#include "playlist/cue/CueParser.cxx"
#include "tag/Item.hxx"
#include "tag/Names.hxx"
#include "util/StaticFifoBuffer.hxx"
#include "util/TextFile.hxx"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <vector>

namespace fs = std::filesystem;

static std::optional<size_t>
InvalidUTF8(std::string_view s)
{
	for (size_t i = 0; i < s.size();) {
		const auto c = static_cast<unsigned char>(s[i]);
		if (c < 0x80) { ++i; continue; }
		const size_t n = c >= 0xc2 && c <= 0xdf ? 2 :
			c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
		if (!n || s.size() - i < n) return i;
		for (size_t j = 1; j < n; ++j)
			if ((static_cast<unsigned char>(s[i+j]) & 0xc0) != 0x80) return i;
		const auto second = static_cast<unsigned char>(s[i+1]);
		if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
		    (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) return i;
		i += n;
	}
	return {};
}

static std::string Quote(std::string_view s)
{
	std::string out = "\"";
	constexpr char hex[] = "0123456789abcdef";
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') { out += '\\'; out += c; }
		else if (c < 0x20) {
			out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
		} else out += c;
	}
	return out + '"';
}

static bool HasScheme(std::string_view s)
{
	auto colon = s.find(':');
	if (colon == s.npos || colon == 0 || !IsAlphaASCII(s.front())) return false;
	return std::all_of(s.begin()+1, s.begin()+colon, [](char c) {
		return IsAlphaASCII(c) || IsDigitASCII(c) || c == '+' || c == '-' || c == '.';
	});
}

static fs::path Resolve(const fs::path &cue, std::string_view name)
{
	fs::path p{name};
	return (p.is_absolute() ? p : cue.parent_path() / p).lexically_normal();
}

int main(int argc, char **argv)
{
	bool json = false;
	const char *filename = nullptr;
	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--help") {
			std::cout << "Usage: cuecheck [--json] [--] FILE.cue\n"; return 0;
		}
		if (arg == "--json" && !json) { json = true; continue; }
		if (arg == "--" && i + 2 == argc && !filename) { filename = argv[++i]; break; }
		if (filename || arg.starts_with('-')) {
			std::cerr << "Usage: cuecheck [--json] [--] FILE.cue\n"; return 2;
		}
		filename = argv[i];
	}
	if (!filename) { std::cerr << "Usage: cuecheck [--json] [--] FILE.cue\n"; return 2; }

	std::vector<std::string> errors;
	std::vector<std::unique_ptr<DetachedSong>> songs;
	fs::path cue = fs::absolute(filename).lexically_normal();
	std::ifstream input(cue, std::ios::binary);
	std::string data;
	if (!input) errors.emplace_back("Cannot open CUE file");
	else {
		try {
			data.assign(std::istreambuf_iterator<char>(input), {});
			if (input.bad()) errors.emplace_back("Cannot read CUE file");
		} catch (const std::ios_base::failure &) {
			errors.emplace_back("Cannot read CUE file");
		}
	}
	if (errors.empty()) {
		if (auto bad = InvalidUTF8(data)) {
			const size_t line = 1 + std::count(data.begin(), data.begin() + *bad, '\n');
			errors.push_back("Invalid UTF-8 at byte " + std::to_string(*bad) +
				" (zero-based), line " + std::to_string(line));
		} else {
			std::string_view remaining = data;
			if (remaining.starts_with("\xef\xbb\xbf")) remaining.remove_prefix(3);
			CueParser parser;
			std::set<std::string> checked;
			auto check_reference = [&](std::string_view name, std::string location) {
				checked.emplace(name);
				std::string error;
				if (HasScheme(name)) error = "Non-local reference cannot be verified";
				else {
					std::error_code ec;
					if (name.empty() || !fs::is_regular_file(Resolve(cue, name), ec))
						error = "Reference is missing or not a regular file";
				}
				if (!error.empty()) errors.push_back(location + ": " + error + ": " + Quote(name));
			};
			StaticFifoBuffer<char, 4096> buffer;
			size_t line_number = 1;
			auto feed = [&](const char *line) {
				std::string_view rest = line;
				if (cue_next_token(rest) == "FILE") {
					auto name = cue_next_value(rest);
					auto type = cue_next_token(rest);
					if (name.data() && type.data())
						check_reference(name, "Line " + std::to_string(line_number));
				}
				parser.Feed(line);
				if (auto song = parser.Get()) songs.push_back(std::move(song));
				line_number += std::count(line, line + std::strlen(line), '\n');
			};
			while (true) {
				if (auto line = ReadBufferedLine(buffer)) { feed(line); ++line_number; continue; }
				buffer.Shift();
				auto dest = buffer.Write();
				if (dest.size() < 2) {
					dest[0] = 0; feed(buffer.Read().data()); buffer.Clear(); continue;
				}
				if (remaining.empty()) {
					dest[0] = 0;
					if (!buffer.Read().empty()) feed(buffer.Read().data());
					break;
				}
				const size_t n = std::min(dest.size()-1, remaining.size());
				std::copy_n(remaining.data(), n, dest.data());
				remaining.remove_prefix(n); buffer.Append(n);
			}
			parser.Finish();
			while (auto song = parser.Get()) songs.push_back(std::move(song));
			for (size_t i = 0; i < songs.size(); ++i)
				if (!checked.contains(songs[i]->GetURI()))
					check_reference(songs[i]->GetURI(), "Track " + std::to_string(i+1));
			if (songs.empty()) errors.emplace_back("No audio tracks parsed");
		}
	}
	if (json) {
		std::cout << "{\"valid\":" << (errors.empty() ? "true" : "false") << ",\"errors\":[";
		for (size_t i = 0; i < errors.size(); ++i) std::cout << (i ? "," : "") << Quote(errors[i]);
		std::cout << "],\"tracks\":[";
	} else {
		std::cout << (errors.empty() ? "PASS: " : "FAIL: ") << filename << '\n';
		for (const auto &error : errors) std::cout << "Error: " << error << '\n';
	}
	for (size_t i = 0; i < songs.size(); ++i) {
		const auto &song = *songs[i];
		auto name = song.GetURI();
		auto end = song.GetEndTime().ToMS();
		const std::string path = HasScheme(name) ? "" : Resolve(cue, name).string();
		if (json) {
			std::cout << (i ? "," : "") << "{\"file\":" << Quote(name) << ",\"path\":" << Quote(path)
				<< ",\"start_ms\":" << song.GetStartTime().ToMS() << ",\"end_ms\":"
				<< (end ? std::to_string(end) : "null") << ",\"tags\":[";
			bool first = true;
			for (const auto &tag : song.GetTag()) {
				std::cout << (first ? "" : ",") << "{\"name\":" << Quote(tag_item_names[tag.type])
					<< ",\"value\":" << Quote(tag.value) << '}'; first = false;
			}
			std::cout << "]}";
		} else {
			std::cout << "Track " << i+1 << ": " << Quote(name) << "\n  Path: " << Quote(path)
				<< "\n  Start: " << song.GetStartTime().ToMS() << " ms\n  End: "
				<< (end ? std::to_string(end) + " ms" : "unspecified") << '\n';
			for (const auto &tag : song.GetTag())
				std::cout << "  " << tag_item_names[tag.type] << ": " << Quote(tag.value) << '\n';
		}
	}
	if (json) std::cout << "]}\n";
	return errors.empty() ? 0 : 1;
}
