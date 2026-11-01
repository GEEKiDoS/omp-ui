#include "chrome.h"

#include "code.h"
#include "core/text_util.h"
#include "gfx/draw.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace kinds {
namespace {

bool token(std::string_view tokens, std::string_view wanted) {
	while (!tokens.empty()) {
		const auto end = tokens.find(' ');
		if (tokens.substr(0, end) == wanted)
			return true;
		if (end == std::string_view::npos)
			break;
		tokens.remove_prefix(end + 1);
	}
	return false;
}

void append(text::Runs& target, text::Runs source) {
	target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

SDL_Color toned(const tsp::Node& node, SDL_Color fallback) {
	text::RunStyle style{.color = fallback};
	text::applyTokens(node.str("tone"), style);
	return style.color;
}

} // namespace

size_t editorRuns(const tsp::Node& node, text::Runs& runs, const text::RunStyle& base, std::vector<EditorSpan>& spans,
                  std::optional<std::pair<size_t, size_t>> selection) {
	spans.clear();
	append(runs, text::toRuns(node.prop("prompt"), {.color = text::palette::accent}));
	size_t visible = 0;
	for (const auto& run : runs)
		visible += run.text.size();
	const auto value = node.str("text");
	const size_t caret = text::byteOffset(value, static_cast<size_t>(std::max(0.0, node.num("cursor"))));
	if (value.empty()) {
		const size_t result = visible;
		append(runs, text::toRuns(node.str("ghost").empty() ? node.prop("placeholder") : node.prop("ghost"),
		                          {.font = base.font, .color = text::palette::dim}));
		return result;
	}
	const size_t anchor = node.prop("anchor").is_number()
	                        ? text::byteOffset(value, static_cast<size_t>(std::max(0.0, node.num("anchor"))))
	                        : caret;
	size_t selectionBegin = std::min(anchor, caret), selectionEnd = std::max(anchor, caret);
	if (selection) {
		selectionBegin = text::byteOffset(value, selection->first);
		selectionEnd = text::byteOffset(value, selection->second);
	}
	struct Decoration {
		size_t begin, end;
		std::string_view tokens;
		int fx;
	};
	std::vector<Decoration> decor;
	// The caret is a run boundary only where ghost text or a selection edge goes: a split word
	// loses its kerning, so the letters after the caret would visibly shift.
	const bool hasGhost = !node.str("ghost").empty() || node.prop("ghost").is_array();
	std::vector<size_t> cuts{0, value.size()};
	if (selectionBegin < selectionEnd) {
		cuts.push_back(selectionBegin);
		cuts.push_back(selectionEnd);
	}
	if (hasGhost)
		cuts.push_back(caret);
	for (const auto& item : node.prop("decor")) {
		const size_t begin = text::byteOffset(value, static_cast<size_t>(std::max(0, item.value("from", 0))));
		const size_t end = text::byteOffset(value, static_cast<size_t>(std::max(0, item.value("to", 0))));
		const auto& tokens = item.at("s").get_ref<const std::string&>();
		const auto fx = item.value("fx", "");
		decor.push_back({begin, end, tokens, fx == "shimmer" ? 1 : fx == "pulse" ? 2 : 0});
		cuts.push_back(begin);
		cuts.push_back(end);
	}
	text::Runs syntax;
	if (!node.str("lang").empty()) {
		size_t offset = 0;
		while (offset < value.size()) {
			const size_t end = value.find('\n', offset);
			const size_t stop = end == std::string_view::npos ? value.size() : end;
			append(syntax, highlightLine(value.substr(offset, stop - offset), node.str("lang")));
			if (end == std::string_view::npos)
				break;
			syntax.push_back({"\n", base});
			offset = end + 1;
		}
	} else
		syntax.push_back({std::string(value), base});
	size_t offset = 0;
	for (const auto& run : syntax) {
		offset += run.text.size();
		cuts.push_back(offset);
	}
	std::sort(cuts.begin(), cuts.end());
	cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
	size_t sourceRun = 0, sourceEnd = syntax.empty() ? value.size() : syntax.front().text.size();
	size_t result = visible;
	for (size_t i = 0; i < cuts.size(); ++i) {
		const size_t begin = cuts[i];
		if (begin == caret) {
			result = visible;
			auto ghost = text::toRuns(node.prop("ghost"), {.font = base.font, .color = text::palette::dim});
			for (const auto& run : ghost)
				visible += run.text.size();
			append(runs, std::move(ghost));
		}
		if (i + 1 == cuts.size())
			break;
		const size_t end = cuts[i + 1];
		if (begin >= end || end > value.size())
			continue;
		while (sourceRun + 1 < syntax.size() && begin >= sourceEnd)
			sourceEnd += syntax[++sourceRun].text.size();
		auto style = syntax.empty() ? base : syntax[sourceRun].style;
		bool hidden = false;
		for (const auto& d : decor)
			if (d.begin <= begin && end <= d.end) {
				hidden |= token(d.tokens, "hide");
				text::applyTokens(d.tokens, style);
				if (d.fx)
					style.fx = d.fx;
			}
		if (selectionBegin < selectionEnd && selectionBegin <= begin && end <= selectionEnd)
			style.bg = text::withAlpha(text::palette::accent, 65);
		if (!hidden) {
			if (begin < caret && caret < end)
				result = visible + (caret - begin);
			spans.push_back({visible, begin, end - begin});
			runs.push_back({std::string(value.substr(begin, end - begin)), std::move(style)});
			visible += end - begin;
		}
	}
	return result;
}

size_t editorSourceOffset(const tsp::Node& node, const std::vector<EditorSpan>& spans, size_t visible) {
	const auto value = node.str("text");
	size_t source = 0;
	for (const auto& span : spans) {
		if (visible < span.visible)
			break; // Before this span (prompt or a hidden stretch): its start.
		source = span.source + std::min(visible - span.visible, span.length);
	}
	return text::utf16Length(value.substr(0, std::min(source, value.size())));
}

SDL_Color cardFill(const tsp::Node& node) {
	return node.str("tone") == "user" ? text::palette::userCard : text::palette::surface;
}

SDL_Color cardBorder(const tsp::Node& node) {
	if (node.flag("selected"))
		return text::palette::accent;
	if (node.str("status") == "error")
		return text::withAlpha(text::palette::error, 180);
	if (node.str("status") == "running")
		return text::withAlpha(text::palette::accent, 155);
	if (node.str("tone").empty())
		return text::palette::border;
	// A tone tints the ring; full-strength tone borders (user turns) dominate the transcript.
	auto border = toned(node, text::palette::border);
	border.a = 70;
	return border;
}

float toastOpacity(const tsp::Node& node, Uint64 arrived) {
	const double age =
	    node.stamps.contains("text") ? node.since("text") : static_cast<double>(SDL_GetTicks() - arrived);
	return static_cast<float>(std::clamp((node.num("ttl", 4000) - age) / 400.0, 0.0, 1.0));
}

void paintMark(const tsp::Node& node, SDL_Renderer& renderer, const SDL_FRect& rect, float scale, bool previousPick,
               bool nextPick) {
	if (node.str("mark") == "drop") {
		gfx::rounded(renderer, rect, 8 * scale, text::withAlpha(text::palette::bg, 165));
	} else if (node.str("mark") == "pick") {
		const auto tint = text::withAlpha(text::palette::accent, 22);
		gfx::rounded(renderer, rect, 8 * scale, tint);
		if (previousPick)
			gfx::fill(renderer, {rect.x, rect.y, rect.w, 8 * scale}, tint);
		if (nextPick)
			gfx::fill(renderer, {rect.x, rect.y + rect.h - 8 * scale, rect.w, 8 * scale}, tint);
		gfx::fill(renderer, {rect.x, rect.y, 3 * scale, rect.h}, text::withAlpha(text::palette::accent, 160));
	}
}

} // namespace kinds
