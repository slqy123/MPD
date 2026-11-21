// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "TakDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "fs/Path.hxx"
#include "io/FileReader.hxx"
#include "lib/fmt/RuntimeError.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "system/Error.hxx"

#include <fooyin_tak.h>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

struct TakInput {
	DecoderClient &client;
	FileReader reader;

	TakInput(DecoderClient &_client, Path path)
		:client(_client), reader(path) {}
};

static int64_t
ReadTak(void *context, uint8_t *buffer, size_t size) noexcept
try {
	auto &input = *static_cast<TakInput *>(context);
	if (input.client.IsStopRequested())
		return -1;

	return input.reader.Read({reinterpret_cast<std::byte *>(buffer), size});
} catch (...) {
	return -1;
}

static int64_t
SeekTak(void *context, int64_t offset, int32_t origin) noexcept
try {
	auto &reader = static_cast<TakInput *>(context)->reader;
	uint64_t base;
	switch (origin) {
	case 0:
		base = 0;
		break;
	case 1:
		base = reader.GetPosition();
		break;
	case 2:
		base = reader.GetSize();
		break;
	default:
		return -1;
	}

	if (!std::in_range<int64_t>(base))
		return -1;

	const auto start = static_cast<int64_t>(base);
	if (offset < -start || offset > std::numeric_limits<int64_t>::max() - start)
		return -1;

	const auto position = start + offset;
	if (!std::in_range<off_t>(position))
		return -1;

	reader.Seek(static_cast<off_t>(position));
	return position;
} catch (...) {
	return -1;
}

static constexpr size_t tak_buffer_samples = 2048;

template<typename T, unsigned shift>
static DecoderCommand
SubmitTak(DecoderClient &client, std::span<const int32_t> samples) noexcept
{
	std::array<T, tak_buffer_samples> pcm;
	std::transform(samples.begin(), samples.end(), pcm.begin(),
		       [](int32_t value) { return static_cast<T>(value >> shift); });
	return client.SubmitAudio(nullptr, std::span{pcm}.first(samples.size()), 0);
}

static void
DecodeTakFile(DecoderClient &client, Path path)
{
	std::optional<TakInput> input;
	try {
		input.emplace(client, path);
	} catch (const std::system_error &e) {
		// An archive member may have been opened by the stream input path.
		if (IsPathNotFound(e))
			return;
		throw;
	}

	TakStreamInfo info{};
	const TakIo io{&*input, ReadTak, SeekTak};
	const std::unique_ptr<TakHandle, decltype(&tak_close)>
		decoder{tak_open(io, &info, nullptr, 0), tak_close};
	if (!decoder || tak_codec(decoder.get()) != 0)
		return;

	SampleFormat format;
	decltype(&SubmitTak<int16_t, 16>) submit;
	switch (info.bits_per_sample) {
	case 8:
		format = SampleFormat::S8;
		submit = SubmitTak<int8_t, 24>;
		break;
	case 16:
		format = SampleFormat::S16;
		submit = SubmitTak<int16_t, 16>;
		break;
	case 24:
		format = SampleFormat::S24_P32;
		submit = SubmitTak<int32_t, 8>;
		break;
	default:
		return;
	}

	const auto audio_format = CheckAudioFormat(info.sample_rate, format, info.channels);
	const SignedSongTime duration = tak_sample_count_known(decoder.get()) == 1
		? SongTime::FromScale(info.total_samples, info.sample_rate)
		: SignedSongTime::Negative();
	client.Ready(audio_format, true, duration);

	std::array<int32_t, tak_buffer_samples> pcm;
	auto command = client.GetCommand();
	while (command != DecoderCommand::STOP) {
		if (command == DecoderCommand::SEEK) {
			if (tak_seek(decoder.get(), client.GetSeekFrame()) < 0) {
				client.SeekError(std::make_exception_ptr(
					FmtRuntimeError("TAK seek failed: {}",
							tak_last_error(decoder.get()))));
				return;
			}
			client.CommandFinished();
		}

		const auto count = tak_read(decoder.get(), pcm.data(), pcm.size());
		if (count < 0 && !client.IsStopRequested())
			throw FmtRuntimeError("TAK decode failed: {}", tak_last_error(decoder.get()));
		if (count <= 0)
			return;

		command = submit(client, std::span{pcm}.first(static_cast<size_t>(count)));
	}
}

static constexpr const char *tak_suffixes[] = {"tak", nullptr};

const DecoderPlugin tak_decoder_plugin =
	DecoderPlugin("tak", DecodeTakFile, nullptr).WithSuffixes(tak_suffixes);
