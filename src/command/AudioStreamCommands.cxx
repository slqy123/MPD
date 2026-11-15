// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "AudioStreamCommands.hxx"
#include "Request.hxx"
#include "SongLoader.hxx"
#include "MusicBuffer.hxx"
#include "MusicPipe.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "client/ThreadBackgroundCommand.hxx"
#include "config/Block.hxx"
#include "decoder/Control.hxx"
#include "encoder/Configured.hxx"
#include "encoder/EncoderInterface.hxx"
#include "pcm/Convert.hxx"
#include "song/DetachedSong.hxx"
#include "lib/fmt/AudioFormatFormatter.hxx"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/**
 * A bounded, pull-driven decoder/encoder session owned by one control
 * connection.  Only the background command accesses the codec objects;
 * Cancel() may additionally be called from the main thread.
 *
 * Use the normal DecoderControl/DecoderBridge, so URI resolution, decoder
 * selection, container tracks, initial seeking and CUE end positions follow
 * the same path as playback.  No PlayerControl or AudioOutput is involved.
 */
class AudioStream {
	Mutex mutex;
	Cond client_cond;
	MusicBuffer buffer{64};
	const std::shared_ptr<MusicPipe> pipe = std::make_shared<MusicPipe>();
	DecoderControl decoder{mutex, client_cond, nullptr,
		AudioFormat::Undefined(), ReplayGainConfig{}};

	std::unique_ptr<DetachedSong> song;
	const SongTime requested_start;
	const SongTime song_start;
	const SongTime song_end;
	std::unique_ptr<PreparedEncoder> prepared_encoder;
	std::unique_ptr<Encoder> encoder;
	std::unique_ptr<PcmConvert> convert;
	MusicChunkPtr pending;

	// Protected by mutex (including while starting the decoder thread).
	bool started = false;
	bool cancelled = false;

	bool ended = false;
	uint64_t byte_offset = 0;
	// Some encoders return an internal page larger than the caller's buffer.
	// Keep its tail until the next pull, without advancing that encoder.
	std::span<const std::byte> encoded_remainder;

public:
	AudioFormat audio_format = AudioFormat::Undefined();
	SongTime start;
	SignedSongTime duration;
	std::string mime;

	AudioStream(DetachedSong &&_song, SongTime offset,
		    std::unique_ptr<PreparedEncoder> _encoder)
		:song(std::make_unique<DetachedSong>(std::move(_song))),
		 requested_start(song->GetStartTime() + offset),
		 song_start(song->GetStartTime()), song_end(song->GetEndTime()),
		 prepared_encoder(std::move(_encoder)),
		 start(offset), duration(song->GetDuration())
	{
		if (uint64_t(song_start.count()) + offset.count() >
		    std::numeric_limits<SongTime::rep>::max())
			throw ProtocolError(ACK_ERROR_ARG, "Start offset too large");

		if (!duration.IsNegative() && offset >= SongTime(duration))
			throw ProtocolError(ACK_ERROR_ARG, "Start offset is beyond the song");

		const char *type = prepared_encoder->GetMimeType();
		mime = type != nullptr ? type : "application/octet-stream";
	}

	~AudioStream() noexcept {
		Cancel();
		if (started)
			decoder.Quit();
		pending.reset();
		pipe->Clear();
	}

	void Cancel() noexcept {
		const std::lock_guard lock{mutex};
		cancelled = true;
		if (started) {
			decoder.command = DecoderCommand::STOP;
			decoder.Signal();
		}
		client_cond.notify_all();
	}

	void Open() {
		{
			std::unique_lock lock{mutex};
			CheckCancelled();
			decoder.StartThread();
			started = true;
			decoder.Start(lock, std::move(song), requested_start, song_end,
				      true, buffer, pipe);
		}

		// Wait for actual audio, so opening reports failed seeks/decoders
		// before the caller commits to an HTTP success response.
		pending = NextChunk();
		AudioFormat source_format;
		{
			const std::lock_guard lock{mutex};
			CheckCancelled();
			source_format = decoder.out_audio_format;
			if (requested_start.IsPositive() && !decoder.seekable)
				throw std::runtime_error("Song is not seekable");
			if (duration.IsNegative() && !decoder.total_time.IsNegative())
				duration = SignedSongTime(int64_t(decoder.total_time.count()) -
							 int64_t(song_start.count()));
		}
		// A URI outside the database may only reveal its duration after
		// decoder initialization. Reject seeking past that newly known end.
		if (!duration.IsNegative() && requested_start - song_start >= SongTime(duration))
			throw ProtocolError(ACK_ERROR_ARG, "Start offset is beyond the song");

		if (pending)
			start = SongTime::FromMS(std::max<int64_t>(0, pending->time.count()));

		audio_format = source_format;
		encoder.reset(prepared_encoder->Open(audio_format));
		if (audio_format != source_format)
			convert = std::make_unique<PcmConvert>(source_format, audio_format);
	}

	struct Chunk {
		uint64_t offset;
		std::vector<std::byte> data;
		bool eof = false;
	};

	Chunk Read(std::size_t limit) {
		Chunk result{byte_offset, std::vector<std::byte>(limit)};
		while (true) {
			{
				const std::lock_guard lock{mutex};
				CheckCancelled();
			}

			const auto encoded = encoded_remainder.empty()
				? encoder->Read(result.data)
				: std::exchange(encoded_remainder, {});
			if (!encoded.empty()) {
				const auto count = std::min(encoded.size(), limit);
				encoded_remainder = encoded.subspan(count);
				// Encoder::Read() may return its own storage.
				if (encoded.data() != result.data.data())
					result.data.assign(encoded.begin(), encoded.begin() + count);
				else
					result.data.resize(count);
				byte_offset += result.data.size();
				return result;
			}

			if (ended) {
				result.data.clear();
				result.eof = true;
				return result;
			}

			auto chunk = pending ? std::move(pending) : NextChunk();
			if (chunk) {
				auto pcm = chunk->ReadData();
				if (convert)
					pcm = convert->Convert(pcm);
				if (!pcm.empty())
					encoder->Write(pcm);
				chunk.reset();
				const std::lock_guard lock{mutex};
				decoder.Signal();
			} else {
				if (convert) {
					while (true) {
						const auto pcm = convert->Flush();
						if (pcm.empty())
							break;
						encoder->Write(pcm);
					}
				}
				encoder->End();
				ended = true;
			}
		}
	}

private:
	// Caller holds mutex.
	void CheckCancelled() const {
		if (cancelled)
			throw std::runtime_error("Audio export cancelled");
	}

	MusicChunkPtr NextChunk() {
		std::unique_lock lock{mutex};
		while (true) {
			CheckCancelled();
			if (decoder.state == DecoderState::ERROR)
				std::rethrow_exception(decoder.GetError());

			if (auto chunk = pipe->Shift()) {
				if (chunk->length > 0)
					return chunk;
				// Tag-only chunks are not part of the exported audio.
				chunk.reset();
				decoder.Signal();
			} else if (decoder.IsIdle()) {
				return {};
			} else {
				decoder.WaitForDecoder(lock);
			}
		}
	}
};

class AudioStreamCommand final : public ThreadBackgroundCommand {
	std::shared_ptr<AudioStream> stream;
	const bool opening;
	const std::size_t limit;
	AudioStream::Chunk chunk;

public:
	AudioStreamCommand(Client &_client, std::shared_ptr<AudioStream> _stream,
			   bool _opening) noexcept
		:ThreadBackgroundCommand(_client, _opening ? "openstream" : "readstream"),
		 stream(std::move(_stream)),
		 opening(_opening), limit(std::min<std::size_t>(_client.binary_limit, 256 * 1024)) {}

protected:
	void Run() override {
		try {
			if (opening)
				stream->Open();
			else
				chunk = stream->Read(limit);
		} catch (...) {
			stream->Cancel();
			throw;
		}
	}

	void SendResponse(Response &r) noexcept override {
		if (opening) {
			r.Fmt("type: {}\nformat: {}\nstart: {:.3f}\n",
			      stream->mime, stream->audio_format, stream->start.ToDoubleS());
			if (!stream->duration.IsNegative())
				r.Fmt("duration: {:.3f}\n", stream->duration.ToDoubleS());
		} else {
			r.Fmt("offset: {}\neof: {}\n", chunk.offset, unsigned(chunk.eof));
			r.WriteBinary(chunk.data);
		}
	}

	void CancelThread() noexcept override {
		stream->Cancel();
	}
};

CommandResult
handle_open_stream(Client &client, Request args, Response &)
{
	if (client.audio_stream)
		throw ProtocolError(ACK_ERROR_EXIST, "Close the previous stream first");

	// Unlike the general SongTime parser, reject NaN/Inf and overflow before
	// converting a floating point value to MPD's millisecond time type.
	const auto offset = args.ParseFloat(1);
	// MPD may be built with -ffast-math, which folds std::isfinite() to
	// true. Check IEEE-754 exponent bits before converting to SongTime.
	if ((std::bit_cast<uint32_t>(offset) & 0x7f800000u) == 0x7f800000u || offset < 0 ||
	    double(offset) * 1000 > std::numeric_limits<SongTime::rep>::max())
		throw ProtocolError(ACK_ERROR_ARG, "Invalid start offset");

	ConfigBlock config;
	const std::string_view name = args[2];
	config.AddBlockParam("encoder", name);
	if (args.size() == 4) {
		const auto bitrate = args.ParseUnsigned(3, 10000);
		if (bitrate == 0)
			throw ProtocolError(ACK_ERROR_ARG, "Bitrate must be positive");
		if (name != "opus" && name != "vorbis" && name != "lame" &&
		    name != "shine" && name != "twolame")
			throw ProtocolError(ACK_ERROR_ARG, "This encoder has no bitrate setting");
		config.AddBlockParam("bitrate", std::to_string(name == "opus"
							     ? bitrate * 1000 : bitrate));
	} else if (name == "lame" || name == "twolame") {
		config.AddBlockParam("bitrate", "192");
	}

	auto prepared = std::unique_ptr<PreparedEncoder>(CreateConfiguredEncoder(config));
	auto stream = std::make_shared<AudioStream>(SongLoader(client).LoadSong(args[0]),
						  SongTime::FromS(offset), std::move(prepared));
	auto command = std::make_unique<AudioStreamCommand>(client, stream, true);
	command->Start();
	client.audio_stream = std::move(stream);
	client.SetBackgroundCommand(std::move(command));
	return CommandResult::BACKGROUND;
}

CommandResult
handle_read_stream(Client &client, Request, Response &)
{
	if (!client.audio_stream)
		throw ProtocolError(ACK_ERROR_NO_EXIST, "No open audio stream");
	if (client.binary_limit == 0)
		throw ProtocolError(ACK_ERROR_ARG, "binarylimit must be positive");

	auto command = std::make_unique<AudioStreamCommand>(client, client.audio_stream, false);
	command->Start();
	client.SetBackgroundCommand(std::move(command));
	return CommandResult::BACKGROUND;
}

CommandResult
handle_close_stream(Client &client, Request, Response &)
{
	client.audio_stream.reset();
	return CommandResult::OK;
}
