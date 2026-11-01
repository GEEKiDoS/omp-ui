#include "spans.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace text {
namespace {

bool contains(std::string_view value, std::string_view word) {
	return std::search(value.begin(), value.end(), word.begin(), word.end(),
	                   [](unsigned char a, unsigned char b) { return std::tolower(a) == b; })
	    != value.end();
}

std::optional<SDL_Color> tokenColor(std::string_view token) {
	using namespace palette;
	if (contains(token, "error") || contains(token, "fail") || contains(token, "removed"))
		return error;
	if (contains(token, "warn"))
		return warning;
	if (contains(token, "success") || contains(token, "added") || contains(token, "cost"))
		return success;
	if (contains(token, "thinking") || contains(token, "muted") || contains(token, "dim") || contains(token, "border")
	    || contains(token, "gray") || contains(token, "context"))
		return muted;
	if (contains(token, "model") || contains(token, "pending"))
		return pending;
	if (contains(token, "info") || contains(token, "path") || contains(token, "cwd"))
		return info;
	if (contains(token, "accent") || contains(token, "title") || contains(token, "link") || contains(token, "heading"))
		return accent;
	if (contains(token, "user"))
		return user;
	return {};
}

bool hasToken(std::string_view tokens, std::string_view target) {
	while (!tokens.empty()) {
		const auto end = tokens.find(' ');
		if (tokens.substr(0, end) == target)
			return true;
		if (end == std::string_view::npos)
			break;
		tokens.remove_prefix(end + 1);
	}
	return false;
}

} // namespace

void usePalette(bool dark) {
	using namespace palette;
	darkPalette = dark;
	if (dark) {
		bg = rgb(0x17191f), surface = rgb(0x1e2129), surface2 = rgb(0x262a34), border = rgb(0x343946);
		fg = rgb(0xd8dce4), muted = rgb(0x8b92a3), dim = rgb(0x5f6676), accent = rgb(0x7aa2f7);
		info = rgb(0x7dcfff), success = rgb(0x9ece6a), warning = rgb(0xe0af68), error = rgb(0xf7768e);
		pending = rgb(0xbb9af7), user = rgb(0xc0caf5);
		hover = rgb(0x232732), userCard = rgb(0x252a37), selected = rgb(0x30405c), focus = rgb(0x536b99);
		primary = rgb(0x3a527a), danger = rgb(0x663044), caution = rgb(0x66502d), mark = rgb(0xffffff);
	} else {
		bg = rgb(0xf6f7f9), surface = rgb(0xffffff), surface2 = rgb(0xeceff3), border = rgb(0xd3d8e0);
		fg = rgb(0x1f2329), muted = rgb(0x5b6472), dim = rgb(0x8a93a1), accent = rgb(0x2f5bd3);
		info = rgb(0x0b76a8), success = rgb(0x2e7d32), warning = rgb(0x996100), error = rgb(0xc92a3a);
		pending = rgb(0x7c4dce), user = rgb(0x34437a);
		hover = rgb(0xeef1f5), userCard = rgb(0xe9effb), selected = rgb(0xd9e4fb), focus = rgb(0x7f9de3);
		primary = rgb(0xc8d7f6), danger = rgb(0xf6d2d8), caution = rgb(0xf4e3c2), mark = rgb(0x1f2329);
	}
	canvas = bg;
	sidebar = surface;
}

std::optional<std::reference_wrapper<SDL_Color>> paletteColor(std::string_view name) {
	using namespace palette;
	// Same order as paletteNames.
	const std::array<std::reference_wrapper<SDL_Color>, paletteNames.size()> entries{
	    bg,      surface, surface2, border,   fg,       muted, dim,     accent, info,    success, warning, error,
	    pending, user,    hover,    userCard, selected, focus, primary, danger, caution, mark,    canvas,  sidebar};
	for (size_t i = 0; i < paletteNames.size(); ++i)
		if (paletteNames[i] == name)
			return entries[i];
	return std::nullopt;
}

void applyTokens(std::string_view tokens, RunStyle& style) {
	int face = style.font == FontId::Icons ? 0 : static_cast<int>(style.font);
	while (!tokens.empty()) {
		const auto end = tokens.find(' ');
		const auto token = tokens.substr(0, end);
		if (token == "muted")
			style.color = palette::muted;
		else if (token == "dim")
			style.color = palette::dim;
		else if (token == "strong")
			face |= 1;
		else if (token == "em")
			face |= 2;
		else if (token == "code") {
			face |= 4;
			style.bg = palette::surface2;
			style.inlineCode = true;
		} else if (token == "mono" || token == "path" || token == "num")
			face |= 4;
		else if (token == "key") {
			face |= 4;
			style.color = palette::accent;
		} else if (token == "link") {
			style.color = palette::accent;
			style.underline = true;
		} else if (token == "ins") {
			style.bg = withAlpha(palette::success, 45);
			style.color = palette::success;
		} else if (token == "del") {
			style.bg = withAlpha(palette::error, 45);
			style.color = palette::error;
			style.strike = true;
		} else if (token == "mark")
			style.bg = withAlpha(palette::warning, 70);
		else if (token == "typo")
			style.wavy = true;
		else if (token == "icon")
			style.icon = true;
		else if (auto color = tokenColor(token))
			style.color = *color;
		if (end == std::string_view::npos)
			break;
		tokens.remove_prefix(end + 1);
	}
	style.font = style.icon ? FontId::Icons : static_cast<FontId>(face);
}

Runs toRuns(const nlohmann::json& value, const RunStyle& base) {
	Runs runs;
	if (value.is_string()) {
		runs.push_back({value.get<std::string>(), base});
	} else if (value.is_array()) {
		runs.reserve(value.size());
		for (const auto& span : value) {
			if (!span.is_object())
				continue;
			const auto content = span.find("t");
			if (content == span.end() || !content->is_string())
				continue;
			const auto tokenProp = span.find("s");
			const std::string_view tokens = tokenProp != span.end() && tokenProp->is_string()
			                                  ? tokenProp->get_ref<const std::string&>()
			                                  : std::string_view{};
			if (hasToken(tokens, "hide"))
				continue;
			RunStyle style = base;
			applyTokens(tokens, style);
			if (const auto fx = span.find("fx"); fx != span.end())
				style.fx = *fx == "shimmer" ? 1 : *fx == "pulse" ? 2 : 0;
			if (const auto href = span.find("href"); href != span.end() && href->is_string()) {
				style.href = href->get<std::string>();
				style.underline = true;
				if (tokens.empty())
					style.color = palette::accent;
			}
			runs.push_back({content->get<std::string>(), std::move(style)});
		}
	}
	return runs;
}

std::string plainText(const nlohmann::json& value) {
	if (value.is_string())
		return value.get<std::string>();
	std::string result;
	if (value.is_array())
		for (const auto& span : value) {
			if (span.is_object())
				if (const auto content = span.find("t"); content != span.end() && content->is_string())
					result += content->get_ref<const std::string&>();
		}
	return result;
}

} // namespace text
