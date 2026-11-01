// UTF-8 / UTF-16 helpers. TSP offsets (`splice`, editor carets, hit ranges)
// count UTF-16 code units, as JavaScript strings do; text is stored as UTF-8.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace text {

/** A filesystem path from UTF-8 text (`std::filesystem::u8path` is deprecated in C++20). */
inline std::filesystem::path utf8Path(std::string_view s) {
	return std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size());
}

/** UTF-8 text of a filesystem path. */
inline std::string pathText(const std::filesystem::path& path) {
	const auto value = path.u8string();
	return {value.begin(), value.end()};
}

/** Decode one code point at `i`, advancing it; malformed bytes decode as U+FFFD. */
inline uint32_t decode(std::string_view s, size_t& i) {
	const auto b0 = static_cast<unsigned char>(s[i]);
	auto cont = [&](size_t at) {
		return at < s.size() && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80;
	};
	if (b0 < 0x80) {
		i += 1;
		return b0;
	}
	if ((b0 & 0xe0) == 0xc0 && cont(i + 1)) {
		uint32_t cp = ((b0 & 0x1fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3fu);
		i += 2;
		return cp;
	}
	if ((b0 & 0xf0) == 0xe0 && cont(i + 1) && cont(i + 2)) {
		uint32_t cp = ((b0 & 0x0fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3fu) << 6)
		            | (static_cast<unsigned char>(s[i + 2]) & 0x3fu);
		i += 3;
		return cp;
	}
	if ((b0 & 0xf8) == 0xf0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
		uint32_t cp = ((b0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3fu) << 12)
		            | ((static_cast<unsigned char>(s[i + 2]) & 0x3fu) << 6)
		            | (static_cast<unsigned char>(s[i + 3]) & 0x3fu);
		i += 4;
		return cp;
	}
	i += 1;
	return 0xfffd;
}

inline void append(std::string& out, uint32_t cp) {
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	} else {
		out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	}
}

/** UTF-16 length of UTF-8 text. */
inline size_t utf16Length(std::string_view s) {
	size_t n = 0;
	for (size_t i = 0; i < s.size();)
		n += decode(s, i) >= 0x10000 ? 2 : 1;
	return n;
}

/** Byte offset of UTF-16 offset `units` (clamped to the end; mid-pair rounds up). */
inline size_t byteOffset(std::string_view s, size_t units) {
	size_t i = 0, n = 0;
	while (i < s.size() && n < units)
		n += decode(s, i) >= 0x10000 ? 2 : 1;
	return i;
}

/** Whether `cp` is a Private Use Area code point (Nerd Font icon glyphs). */
inline bool isPua(uint32_t cp) {
	return (cp >= 0xe000 && cp <= 0xf8ff) || (cp >= 0xf0000 && cp <= 0x10fffd);
}

} // namespace text
