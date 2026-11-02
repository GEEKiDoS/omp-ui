#pragma once

#include "text_util.h"
#include <string>
#include <string_view>

namespace gfx {

inline std::string iconGlyph(std::string_view name) {
	if (name.empty())
		return {};
	size_t at = 0;
	if (text::isPua(text::decode(name, at)))
		return std::string(name);
	struct Entry {
		std::string_view key;
		uint32_t cp;
	};
	static constexpr Entry icons[] = {
	    {"lightbulb", 0xf0eb},
	    {"chev", 0xf078},
	    {"chevron", 0xf078},
	    {"chev-up", 0xf077},
	    {"chev-r", 0xf054},
	    {"commit", 0xf417},
	    {"corner-down-right", 0xf060d},
	    {"type", 0xf031},
	    {"wand", 0xf0d0},
	    {"check", 0xf00c},
	    {"close", 0xf00d},
	    {"x", 0xf00d},
	    {"warning", 0xf071},
	    {"warn", 0xf071},
	    {"info", 0xf129},
	    {"error", 0xf057},
	    {"folder", 0xf07b},
	    {"dir", 0xf07b},
	    {"file", 0xf15b},
	    {"search", 0xf002},
	    {"grep", 0xf002},
	    {"find", 0xf002},
	    {"glob", 0xf002},
	    {"terminal", 0xf120},
	    {"bash", 0xf120},
	    {"shell", 0xf120},
	    {"pwsh", 0xf120},
	    {"gear", 0xf013},
	    {"settings", 0xf013},
	    {"clock", 0xf017},
	    {"time", 0xf017},
	    {"branch", 0xe725},
	    {"git", 0xe702},
	    {"robot", 0xf06a9},
	    {"agent", 0xf06a9},
	    {"task", 0xf06a9},
	    {"model", 0xf09d1},
	    {"brain", 0xf09d1},
	    {"thinking", 0xf09d1},
	    {"download", 0xf019},
	    {"plug", 0xf1e6},
	    {"cpu", 0xf2db},
	    {"mcp", 0xf1e6},
	    {"user", 0xf007},
	    {"globe", 0xf0ac},
	    {"web", 0xf0ac},
	    {"fetch", 0xf0ac},
	    {"browser", 0xf0ac},
	    {"copy", 0xf0c5},
	    {"play", 0xf04b},
	    {"stop", 0xf04d},
	    {"pause", 0xf04c},
	    {"refresh", 0xf021},
	    {"lock", 0xf023},
	    {"key", 0xf084},
	    {"star", 0xf005},
	    {"tag", 0xf02b},
	    {"bolt", 0xf0e7},
	    {"speculation", 0xf0e7},
	    {"edit", 0xf040},
	    {"write", 0xf040},
	    {"pencil", 0xf040},
	    {"trash", 0xf1f8},
	    {"list", 0xf03a},
	    {"todo", 0xf0ae},
	    {"plus", 0xf067},
	    {"minus", 0xf068},
	    {"question", 0xf128},
	    {"help", 0xf128},
	    {"bell", 0xf0f3},
	    {"cloud", 0xf0c2},
	    {"code", 0xf121},
	    {"eval", 0xf121},
	    {"link", 0xf0c1},
	    {"eye", 0xf06e},
	    {"read", 0xf06e},
	    {"book", 0xf02d},
	    {"bug", 0xf188},
	    {"cube", 0xf1b2},
	    {"dot", 0xf111},
	    {"compaction", 0xf187},
	    {"archive", 0xf187},
	    {"session", 0xf086},
	    {"chat", 0xf086},
	    {"history", 0xf1da},
	    {"image", 0xf03e},
	    {"memory", 0xf0eb},
	    {"cost", 0xf155},
	    {"usage", 0xf080},
	    {"chart", 0xf080},
	    {"context", 0xf1c0},
	    {"home", 0xf015},
	    {"appearance", 0xf1fc},
	    {"interaction", 0xf11c},
	    {"files", 0xf0c5},
	    {"tools", 0xf0ad},
	    {"tasks", 0xf03a},
	    {"providers", 0xf233},
	    {"plugins", 0xf1b2},
	    // Names omp sends in transcripts, completions and pickers (Font Awesome 4 range of the Nerd font).
	    {"action", 0xf0e7},
	    {"advisor", 0xf19d},
	    {"agents", 0xf0c0},
	    {"broadcast", 0xf0a1},
	    {"cart", 0xf07a},
	    {"clipboard", 0xf0ea},
	    {"compass", 0xf14e},
	    {"compress", 0xf066},
	    {"computer", 0xf108},
	    {"eraser", 0xf12d},
	    {"expand", 0xf065},
	    {"export", 0xf08e},
	    {"extension", 0xf12e},
	    {"fast", 0xf050},
	    {"folderMinus", 0xf114},
	    {"folderMove", 0xf07c},
	    {"folderPlus", 0xf07c},
	    {"gauge", 0xf0e4},
	    {"ghost", 0xf21b},
	    {"goal", 0xf140},
	    {"hammer", 0xf0e3},
	    {"handoff", 0xf0ec},
	    {"host", 0xf233},
	    {"inbox", 0xf01c},
	    {"jobs", 0xf0ae},
	    {"keyboard", 0xf11c},
	    {"loop", 0xf01e},
	    {"news", 0xf1ea},
	    {"package", 0xf187},
	    {"pin", 0xf08d},
	    {"plan", 0xf0cb},
	    {"power", 0xf011},
	    {"prewalk", 0xf018},
	    {"prompt", 0xf0f6},
	    {"redo", 0xf01e},
	    {"restart", 0xf01e},
	    {"rewind", 0xf04a},
	    {"rocket", 0xf135},
	    {"rule", 0xf0cb},
	    {"share", 0xf064},
	    {"shield", 0xf132},
	    {"signIn", 0xf090},
	    {"signOut", 0xf08b},
	    {"skill", 0xf0d0},
	    {"sparkles", 0xf0d0},
	    {"stats", 0xf080},
	    {"stethoscope", 0xf0f1},
	    {"success", 0xf00c},
	    {"swap", 0xf0ec},
	    {"tree", 0xf1bb},
	    {"vibrate", 0xf10b},
	    {"voice", 0xf130},
	    {"wave", 0xf256}};
	auto lookup = [](std::string_view key) {
		for (const auto& icon : icons)
			if (key == icon.key) {
				std::string glyph;
				text::append(glyph, icon.cp);
				return glyph;
			}
		return std::string{};
	};
	if (auto glyph = lookup(name); !glyph.empty())
		return glyph;
	// Match the most specific segment first: omp.tool.grep becomes grep.
	while (!name.empty()) {
		const auto cut = name.find_last_of(".:/-_");
		if (auto glyph = lookup(cut == std::string_view::npos ? name : name.substr(cut + 1)); !glyph.empty())
			return glyph;
		if (cut == std::string_view::npos)
			break;
		name = name.substr(0, cut);
	}
	return {};
}

} // namespace gfx
