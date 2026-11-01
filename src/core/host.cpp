#include "host.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <cstdlib>

namespace {

std::optional<unsigned> parameter(std::string_view value, unsigned fallback = 0) {
	if (value.empty())
		return fallback;
	unsigned result = 0;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
	if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
		return {};
	return result;
}

bool osc99Notification(std::string_view body) {
	const auto semi = body.find(';');
	if (semi == std::string_view::npos)
		return false;
	auto metadata = body.substr(0, semi);
	while (!metadata.empty()) {
		const auto colon = metadata.find(':');
		const auto field = metadata.substr(0, colon);
		// Only the final title/body chunk delivers a notification, not probes
		// or control payloads such as p=? and p=close.
		if (field == "d=0" || (field.starts_with("p=") && field != "p=title" && field != "p=body"))
			return false;
		if (colon == std::string_view::npos)
			break;
		metadata.remove_prefix(colon + 1);
	}
	return true;
}

} // namespace
TerminalHost::TerminalHost(WriteFn write) : session(write), write_(std::move(write)), parser_(*this) {
	// The whole v1 vocabulary but the kinds this renderer cannot draw (the media
	// viewer, typeset math): omp describes those differently.
	session.kinds = {"col",       "row",     "card",    "section",  "rule",   "spacer", "text",   "md",    "code",
	                 "diff",      "ansi",    "kv",      "table",    "tree",   "json",   "badge",  "kbd",   "icon",
	                 "spinner",   "shimmer", "elapsed", "progress", "rate",   "list",   "item",   "tabs",  "editor",
	                 "input",     "status",  "seg",     "overlay",  "toast",  "rows",   "picker", "prefs", "tool",
	                 "checklist", "agent",   "chart",   "meter",    "effort", "image"};
	session.features = {"scroll", "aside"};
}

void TerminalHost::onText(std::string_view text) {
	for (char ch : text) {
		const auto b = static_cast<unsigned char>(ch);
		if (b == '\n' || b == '\t' || b >= 0x20)
			rowsText.push_back(ch);
	}
	if (rowsText.size() > rowsLimit) {
		size_t cut = rowsText.size() - rowsLimit / 2;
		while (cut < rowsText.size() && (static_cast<unsigned char>(rowsText[cut]) & 0xc0) == 0x80)
			cut++;
		rowsText.erase(0, cut);
	}
}

void TerminalHost::onCsi(char prefix, std::string_view params, std::string_view intermediates, char final) {
	if (!intermediates.empty())
		return; // DECRQM (`$p`), cursor style (` q`), …: unanswered, the DA1 sentinel covers them.
	switch (final) {
	case 'c':
		if (prefix == 0 && (params.empty() || params == "0")) {
			write_("\x1b[?62;22c"); // VT220 with ANSI colour.
		} else if (prefix == '>' && (params.empty() || params == "0")) {
			write_("\x1b[>0;10;1c");
		}
		return;
	case 'n':
		if (prefix == 0 && params == "5")
			write_("\x1b[0n");
		else if (prefix == 0 && params == "6")
			write_("\x1b[1;1R");
		return;
	case 't': {
		auto& s = session;
		if (params == "14") {
			write_("\x1b[4;" + std::to_string(static_cast<int>(s.cellH * rows)) + ";"
			       + std::to_string(static_cast<int>(s.cellW * s.cols)) + "t");
		} else if (params == "16") {
			write_("\x1b[6;" + std::to_string(static_cast<int>(s.cellH)) + ";"
			       + std::to_string(static_cast<int>(s.cellW)) + "t");
		} else if (params == "18") {
			write_("\x1b[8;" + std::to_string(rows) + ";" + std::to_string(s.cols) + "t");
		}
		return;
	}
	case 'u': {
		if (prefix == '?' && params.empty()) {
#ifndef _WIN32
			write_("\x1b[?" + std::to_string(kittyFlags) + "u");
#endif
			return;
		}
		const auto semi = params.find(';');
		const auto value = parameter(params.substr(0, semi), prefix == '<' ? 1 : 0);
		if (!value)
			return;
		auto& keyboard = kitty_[alternateScreen_];
		if (prefix == '>') {
			if (semi != std::string_view::npos)
				return;
			// The kitty stack may discard its oldest entry when it fills.
			if (keyboard.depth == keyboard.stack.size()) {
				std::move(keyboard.stack.begin() + 1, keyboard.stack.end(), keyboard.stack.begin());
				--keyboard.depth;
			}
			keyboard.stack[keyboard.depth++] = kittyFlags;
			kittyFlags = *value & 1;
		} else if (prefix == '<') {
			if (semi != std::string_view::npos)
				return;
			if (*value > keyboard.depth) {
				keyboard.depth = 0;
				kittyFlags = 0;
			} else {
				for (unsigned count = *value; count; --count)
					kittyFlags = keyboard.stack[--keyboard.depth];
			}
		} else if (prefix == '=') {
			const auto mode =
			    parameter(semi == std::string_view::npos ? std::string_view{} : params.substr(semi + 1), 1);
			if (!mode)
				return;
			if (*mode == 1)
				kittyFlags = *value & 1;
			else if (*mode == 2)
				kittyFlags |= *value & 1;
			else if (*mode == 3)
				kittyFlags &= ~(*value & 1);
		}
		keyboard.flags = kittyFlags;
		return;
	}
	case 'm':
		if (prefix == '>') {
			const auto semi = params.find(';');
			if (params.substr(0, semi) != "4")
				return;
			const auto level = parameter(semi == std::string_view::npos ? std::string_view{} : params.substr(semi + 1));
			if (level && *level <= 2)
				modifyOtherKeys = *level;
		}
		return;
	case 'h':
	case 'l':
		if (prefix != '?')
			return;
		// Modes may come several at once: `CSI ? 1000 ; 1006 h`.
		for (size_t at = 0; at <= params.size();) {
			size_t semi = params.find(';', at);
			if (semi == std::string_view::npos)
				semi = params.size();
			int mode = std::atoi(std::string(params.substr(at, semi - at)).c_str());
			if (mode == 9001)
				win32InputMode = final == 'h';
			if (mode == 2004)
				bracketedPaste = final == 'h';
			if (mode == 47 || mode == 1047 || mode == 1049) {
				alternateScreen_ = final == 'h';
				kittyFlags = kitty_[alternateScreen_].flags;
			}
			at = semi + 1;
		}
		return;
	default:
		return;
	}
}

void TerminalHost::onOsc(std::string_view body) {
	size_t semi = body.find(';');
	std::string_view code = body.substr(0, semi);
	std::string_view rest = semi == std::string_view::npos ? std::string_view() : body.substr(semi + 1);
	if (code == "0" || code == "2") {
		title = std::string(rest);
	} else if ((code == "10" || code == "11") && rest == "?") {
		write_("\x1b]" + std::string(code) + ";" + (code == "10" ? foreground : background) + "\x1b\\");
	} else if (onNotification && semi != std::string_view::npos
	           && ((code == "9" && rest != "4" && !rest.starts_with("4;"))
	               || (code == "777" && rest.starts_with("notify;")) || (code == "99" && osc99Notification(rest)))) {
		onNotification();
	}
}

void TerminalHost::onApc(std::string_view body) {
	session.onApc(body);
}
