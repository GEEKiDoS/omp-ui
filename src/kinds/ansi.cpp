#include "ansi.h"

#include "core/text_util.h"
#include "gfx/draw.h"
#include "view/hit_targets.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <utility>

namespace kinds {
namespace {

constexpr float naturalLimit = 1000000;

SDL_Color palette16(int index) {
	constexpr std::array<unsigned, 16> colors{0x2a2e38, 0xf7768e, 0x9ece6a, 0xe0af68, 0x7aa2f7, 0xbb9af7,
	                                          0x7dcfff, 0xc0c6d4, 0x5f6676, 0xff8fa3, 0xb9f27c, 0xffc777,
	                                          0x8fb4ff, 0xd0b3ff, 0xa4e0ff, 0xeef0f6};
	return text::rgb(colors[static_cast<size_t>(index)]);
}

SDL_Color palette256(int index) {
	index = std::clamp(index, 0, 255);
	if (index < 16)
		return palette16(index);
	if (index < 232) {
		index -= 16;
		constexpr std::array<Uint8, 6> steps{0, 95, 135, 175, 215, 255};
		return {steps[static_cast<size_t>(index / 36)], steps[static_cast<size_t>((index / 6) % 6)],
		        steps[static_cast<size_t>(index % 6)], 255};
	}
	const auto value = static_cast<Uint8>(8 + (index - 232) * 10);
	return {value, value, value, 255};
}

struct Parameter {
	int value = 0;
	char separator = 0;
};

Parameter parameter(std::string_view values, size_t& at) {
	const size_t found = values.find_first_of(";:", at);
	const size_t end = found == std::string_view::npos ? values.size() : found;
	Parameter result;
	if (at < end)
		std::from_chars(values.data() + at, values.data() + end, result.value);
	if (end < values.size())
		result.separator = values[end];
	at = end + 1;
	return result;
}

void applySgr(AnsiState& state, std::string_view values) {
	for (size_t at = 0; at <= values.size();) {
		const int value = parameter(values, at).value;
		if (value == 0) {
			state.foreground = {};
			state.background = {};
			state.bold = state.dim = state.italic = state.underline = state.inverse = state.strike = false;
		} else if (value == 1)
			state.bold = true;
		else if (value == 2)
			state.dim = true;
		else if (value == 3)
			state.italic = true;
		else if (value == 4 || value == 21)
			state.underline = true;
		else if (value == 7)
			state.inverse = true;
		else if (value == 9)
			state.strike = true;
		else if (value == 22)
			state.bold = state.dim = false;
		else if (value == 23)
			state.italic = false;
		else if (value == 24)
			state.underline = false;
		else if (value == 27)
			state.inverse = false;
		else if (value == 29)
			state.strike = false;
		else if (value >= 30 && value <= 37)
			state.foreground = palette16(value - 30);
		else if (value == 39)
			state.foreground = {};
		else if (value >= 40 && value <= 47)
			state.background = palette16(value - 40);
		else if (value == 49)
			state.background = {};
		else if (value >= 90 && value <= 97)
			state.foreground = palette16(value - 90 + 8);
		else if (value >= 100 && value <= 107)
			state.background = palette16(value - 100 + 8);
		else if ((value == 38 || value == 48) && at <= values.size()) {
			const auto mode = parameter(values, at);
			SDL_Color color{};
			if (mode.value == 5 && at <= values.size())
				color = palette256(parameter(values, at).value);
			else if (mode.value == 2 && at <= values.size()) {
				auto red = parameter(values, at);
				if (at > values.size())
					continue;
				auto green = parameter(values, at);
				if (at > values.size())
					continue;
				auto blue = parameter(values, at);
				// Colon notation may include an empty (or zero) colour-space field.
				if (mode.separator == ':' && blue.separator == ':' && at <= values.size()) {
					red = green;
					green = blue;
					blue = parameter(values, at);
				}
				color = {static_cast<Uint8>(std::clamp(red.value, 0, 255)),
				         static_cast<Uint8>(std::clamp(green.value, 0, 255)),
				         static_cast<Uint8>(std::clamp(blue.value, 0, 255)), 255};
			}
			if (color.a != 0)
				(value == 38 ? state.foreground : state.background) = color;
		}
	}
}

text::RunStyle runStyle(const AnsiState& state) {
	text::RunStyle result;
	result.font = state.bold ? (state.italic ? text::FontId::MonoBoldItalic : text::FontId::MonoBold)
	                         : (state.italic ? text::FontId::MonoItalic : text::FontId::Mono);
	result.color = state.foreground.a ? state.foreground : text::palette::fg;
	result.bg = state.background;
	if (state.inverse) {
		result.bg = result.color;
		result.color = state.background.a ? state.background : text::palette::bg;
	}
	if (state.dim)
		result.color.a = static_cast<Uint8>(static_cast<unsigned>(result.color.a) * 3 / 5);
	result.underline = state.underline || !state.href.empty();
	result.strike = state.strike;
	result.href = state.href;
	return result;
}

bool sameColor(SDL_Color left, SDL_Color right) {
	return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

void append(text::Runs& runs, std::string_view value, const text::RunStyle& style) {
	if (!runs.empty()) {
		auto& previous = runs.back();
		if (previous.style.font == style.font && sameColor(previous.style.color, style.color)
		    && sameColor(previous.style.bg, style.bg) && previous.style.underline == style.underline
		    && previous.style.strike == style.strike && previous.style.href == style.href) {
			previous.text.append(value);
			return;
		}
	}
	runs.push_back({std::string(value), style});
}

void backspace(text::Runs& runs) {
	if (runs.empty())
		return;
	auto& value = runs.back().text;
	size_t at = value.size() - 1;
	while (at > 0 && (static_cast<unsigned char>(value[at]) & 0xc0) == 0x80)
		--at;
	value.resize(at);
	if (value.empty())
		runs.pop_back();
}

struct LineEnd {
	size_t at;
	bool newline = false;
};

LineEnd parseLine(std::string_view source, size_t at, AnsiState& state, AnsiLine& line) {
	line.runs.clear();
	line.layoutWidth = line.naturalWidth = -1;
	auto style = runStyle(state);
	size_t column = 0;
	while (at < source.size()) {
		const char value = source[at];
		if (value == '\x1b') {
			if (at + 1 == source.size())
				break;
			if (source[at + 1] == '[') {
				size_t end = at + 2;
				while (end < source.size() && !(source[end] >= 0x40 && source[end] <= 0x7e))
					++end;
				if (end == source.size())
					break;
				if (source[end] == 'm') {
					applySgr(state, source.substr(at + 2, end - at - 2));
					style = runStyle(state);
				}
				at = end + 1;
				continue;
			}
			if (source[at + 1] == ']') {
				size_t end = at + 2, terminator = 0;
				for (; end < source.size(); ++end) {
					if (source[end] == '\x07') {
						terminator = 1;
						break;
					}
					if (source[end] == '\x1b' && end + 1 < source.size() && source[end + 1] == '\\') {
						terminator = 2;
						break;
					}
				}
				if (terminator == 0)
					break;
				const auto body = source.substr(at + 2, end - at - 2);
				if (body.starts_with("8;")) {
					const size_t separator = body.find(';', 2);
					if (separator != std::string_view::npos) {
						state.href.assign(body.substr(separator + 1));
						style = runStyle(state);
					}
				}
				at = end + terminator;
				continue;
			}
			at += 2;
			continue;
		}
		if (value == '\n')
			return {at + 1, true};
		if (value == '\r') {
			if (at + 1 == source.size() || source[at + 1] != '\n') {
				line.runs.clear();
				column = 0;
			}
			++at;
			continue;
		}
		if (value == '\b') {
			backspace(line.runs);
			if (column > 0)
				--column;
			++at;
			continue;
		}
		if (value == '\t') {
			const size_t spaces = 8 - column % 8;
			append(line.runs, std::string_view("        ").substr(0, spaces), style);
			column += spaces;
			++at;
			continue;
		}
		if (static_cast<unsigned char>(value) < 0x20 || value == '\x7f') {
			++at;
			continue;
		}
		const size_t begin = at;
		do {
			text::decode(source, at);
			++column;
		} while (at < source.size() && static_cast<unsigned char>(source[at]) >= 0x20 && source[at] != '\x7f');
		append(line.runs, source.substr(begin, at - begin), style);
	}
	return {source.size()};
}

struct Metrics {
	float lineHeight, ascent;
};

Metrics metrics(const text::Fonts& fonts) {
	Metrics result{fonts.lineHeight(), static_cast<float>(TTF_GetFontAscent(&fonts.get(text::FontId::Ui)))};
	for (const auto id :
	     {text::FontId::Mono, text::FontId::MonoBold, text::FontId::MonoItalic, text::FontId::MonoBoldItalic}) {
		result.lineHeight = std::max(result.lineHeight, static_cast<float>(TTF_GetFontLineSkip(&fonts.get(id))));
		result.ascent = std::max(result.ascent, static_cast<float>(TTF_GetFontAscent(&fonts.get(id))));
	}
	return result;
}

void uniformMetrics(text::Layout& layout, float lineHeight, float ascent) {
	layout.lineHeight = lineHeight;
	layout.ascent = ascent;
	layout.height = static_cast<float>(layout.lineWidths.size()) * lineHeight;
}

float naturalWidth(AnsiLine& line, const text::Fonts& fonts) {
	if (line.naturalWidth < 0) {
		line.naturalWidth = 0;
		for (const auto& run : line.runs)
			line.naturalWidth += fonts.width(run.style.font, run.text);
	}
	return line.naturalWidth;
}

void layoutLine(AnsiLine& line, text::Engine& engine, float width, text::Wrap wrap, float lineHeight, float ascent) {
	if (line.layoutWidth == width)
		return;
	line.layout =
	    engine.layout(line.runs, {.width = width, .wrap = wrap, .maxLines = wrap == text::Wrap::None ? 1 : 0});
	uniformMetrics(line.layout, lineHeight, ascent);
	line.layoutWidth = width;
}

void links(const AnsiLine& line, float x, float y, const SDL_FRect& clip, view::HitTargets& hits) {
	for (const auto& piece : line.layout.pieces) {
		const auto& href = line.runs[piece.run].style.href;
		if (href.empty() || piece.ellipsis || piece.width <= 0)
			continue;
		hits.add(gfx::intersect({x + piece.x, y + static_cast<float>(piece.line) * line.layout.lineHeight, piece.width,
		                         line.layout.lineHeight},
		                        clip),
		         view::TargetKind::Link, href);
	}
}

} // namespace

void syncAnsi(const tsp::Node& node, AnsiCache& cache, text::Engine&, const text::Fonts& fonts, bool chromeless) {
	if (cache.fontSize != fonts.size()) {
		const auto value = metrics(fonts);
		cache.fontSize = fonts.size();
		cache.lineHeight = value.lineHeight;
		cache.ascent = value.ascent;
		cache.dirtyFrom = cache.naturalDirtyFrom = 0;
		for (auto& line : cache.lines)
			line.layoutWidth = line.naturalWidth = -1;
		cache.contentWidth = -1;
	}
	cache.chromeless = chromeless;
	cache.padX = chromeless ? 0 : fonts.size() * gfx::space::lg;
	cache.padY = cache.padX;
	cache.follow = node.flag("follow");
	const auto& preview = node.prop("preview");
	cache.previewLines = preview.is_null() ? 0 : 10;
	if (preview.is_object()) {
		const auto lines = preview.find("lines");
		if (lines != preview.end() && lines->is_number())
			cache.previewLines = std::max(0, static_cast<int>(lines->get<double>()));
	}
	const auto source = node.str("text");
	if (cache.lines.empty() || source != cache.source) {
		if (!cache.lines.empty() && source.starts_with(cache.source))
			cache.source.append(source.substr(cache.source.size()));
		else {
			cache.source.assign(source);
			cache.lastLine = cache.lastLineStart = 0;
			cache.lastLineState = {};
		}
		cache.dirtyFrom = std::min(cache.dirtyFrom, cache.lastLine);
		cache.naturalDirtyFrom = std::min(cache.naturalDirtyFrom, cache.lastLine);
		cache.lines.resize(cache.lastLine + 1);
		auto state = cache.lastLineState;
		size_t at = cache.lastLineStart;
		for (;;) {
			const auto end = parseLine(cache.source, at, state, cache.lines.back());
			if (!end.newline)
				break;
			cache.lastLineStart = at = end.at;
			cache.lastLineState = state;
			cache.lastLine = cache.lines.size();
			cache.lines.emplace_back();
		}
	}
}

SDL_FPoint measureAnsi(const tsp::Node& node, AnsiCache& cache, text::Engine& engine, const text::Fonts& fonts,
                       float width, bool expanded) {
	cache.preferredWidth = width;
	cache.expanded = expanded;
	float available = std::max(0.0f, width);
	if (width >= naturalLimit) {
		for (size_t i = cache.naturalDirtyFrom; i < cache.lines.size(); ++i)
			cache.lines[i].widest =
			    std::max(i == 0 ? 0 : cache.lines[i - 1].widest, naturalWidth(cache.lines[i], fonts));
		cache.naturalDirtyFrom = cache.lines.size();
		float natural = cache.lines.empty() ? 0 : cache.lines.back().widest;
		const float columns = static_cast<float>(node.num("cols"));
		if (columns > 0)
			natural = std::min(natural, columns * fonts.monoAdvance());
		available = natural + cache.padX * 2;
	}
	const float contentWidth = std::max(0.0f, available - cache.padX * 2);
	const bool widthChanged = cache.contentWidth != contentWidth;
	cache.contentWidth = contentWidth;
	cache.lineCount = cache.lines.size();
	if (cache.lineCount > 1 && cache.lines.back().runs.empty())
		--cache.lineCount;
	// A collapsed preview shows only the last lines: earlier ones get no layout (they are laid
	// out once expanded), and tops count from the first shown line.
	const size_t hidden =
	    !expanded && cache.previewLines > 0 && cache.lineCount > static_cast<size_t>(cache.previewLines)
	        ? cache.lineCount - static_cast<size_t>(cache.previewLines)
	        : 0;
	// Lines from `changed` on have new content; hidden ones just lose their stale layout.
	const size_t changed = std::min(cache.dirtyFrom, cache.lines.size());
	for (size_t i = changed; i < hidden; ++i)
		cache.lines[i].layoutWidth = -1;
	const size_t start = widthChanged || hidden != cache.hiddenLines ? hidden : std::max(hidden, changed);
	for (size_t i = start; i < cache.lines.size(); ++i) {
		auto& line = cache.lines[i];
		if (i >= changed || line.layoutWidth != contentWidth)
			layoutLine(line, engine, contentWidth, text::Wrap::Char, cache.lineHeight, cache.ascent);
		line.top = i == hidden ? 0 : cache.lines[i - 1].top + cache.lines[i - 1].layout.height;
	}
	cache.dirtyFrom = cache.lines.size();
	cache.contentHeight = cache.lineCount == 0
	                        ? 0
	                        : cache.lines[cache.lineCount - 1].top + cache.lines[cache.lineCount - 1].layout.height;
	if (hidden > 0) {
		const bool labelChanged = hidden != cache.hiddenLines || cache.earlier.empty();
		if (labelChanged) {
			text::RunStyle style;
			style.font = text::FontId::Mono;
			style.color = text::palette::dim;
			cache.earlier = {{"… " + std::to_string(hidden) + " earlier lines", std::move(style)}};
		}
		if (labelChanged || widthChanged || cache.earlierLayout.lineHeight != cache.lineHeight) {
			cache.earlierLayout =
			    engine.layout(cache.earlier, {.width = contentWidth, .wrap = text::Wrap::None, .maxLines = 1});
			uniformMetrics(cache.earlierLayout, cache.lineHeight, cache.ascent);
		}
	}
	cache.hiddenLines = hidden;
	const float hiddenHeight = 0;
	cache.measuredWidth = available;
	cache.measuredHeight = cache.contentHeight - hiddenHeight
	                     + (hidden > 0 ? cache.lineHeight + fonts.size() * gfx::space::sm : 0) + cache.padY * 2;
	return {cache.measuredWidth, cache.measuredHeight};
}

bool paintAnsi(const tsp::Node& node, AnsiCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, bool expanded, view::HitTargets& hits) {
	measureAnsi(node, cache, engine, fonts, rect.w, expanded);
	if (!cache.chromeless)
		gfx::rounded(renderer, rect, fonts.size() / 3, text::palette::surface2);
	const SDL_FRect inner{rect.x + cache.padX, rect.y + cache.padY, std::max(0.0f, rect.w - cache.padX * 2),
	                      std::max(0.0f, rect.h - cache.padY * 2)};
	gfx::ScopedClip clip(renderer, inner);
	if (clip.visible.w <= 0 || clip.visible.h <= 0)
		return false;
	const float banner = cache.hiddenLines > 0 ? cache.lineHeight + fonts.size() * gfx::space::sm : 0;
	if (banner > 0) {
		engine.draw(cache.earlier, cache.earlierLayout, inner.x, inner.y);
		hits.add(gfx::intersect({inner.x, inner.y, inner.w, banner}, clip.visible), view::TargetKind::Expand);
	}
	const float hiddenHeight = cache.hiddenLines == 0 ? 0 : cache.lines[cache.hiddenLines].top;
	const float tail =
	    cache.follow ? std::max(0.0f, cache.contentHeight - hiddenHeight - std::max(0.0f, inner.h - banner)) : 0;
	const float origin = inner.y + banner - hiddenHeight - tail;
	const SDL_FRect body{inner.x, inner.y + banner, inner.w, std::max(0.0f, inner.h - banner)};
	gfx::ScopedClip bodyClip(renderer, body);
	if (bodyClip.visible.w <= 0 || bodyClip.visible.h <= 0)
		return false;
	const auto begin = cache.lines.begin() + static_cast<std::ptrdiff_t>(cache.hiddenLines);
	const auto end = cache.lines.begin() + static_cast<std::ptrdiff_t>(cache.lineCount);
	const auto first = std::lower_bound(begin, end, bodyClip.visible.y, [&](const AnsiLine& line, float top) {
		return origin + line.top + line.layout.height <= top;
	});
	for (auto line = first; line != end && origin + line->top < bodyClip.visible.y + bodyClip.visible.h; ++line) {
		engine.draw(line->runs, line->layout, inner.x, origin + line->top);
		links(*line, inner.x, origin + line->top, bodyClip.visible, hits);
	}
	return false;
}

void forgetAnsi(AnsiCache& cache, text::Engine& engine) {
	for (const auto& line : cache.lines)
		engine.forget(line.runs);
	engine.forget(cache.earlier);
	cache = {};
}

void syncRows(const tsp::Node& node, RowsCache& cache, text::Engine&, const text::Fonts& fonts, bool) {
	if (cache.fontSize != fonts.size()) {
		const auto value = metrics(fonts);
		cache.fontSize = fonts.size();
		cache.lineHeight = value.lineHeight;
		cache.ascent = value.ascent;
		cache.dirtyFrom = cache.naturalDirtyFrom = 0;
		for (auto& row : cache.rows)
			row.line.layoutWidth = row.line.naturalWidth = -1;
		cache.contentWidth = -1;
	}
	const auto& lines = node.prop("lines");
	const size_t count = lines.is_array() ? lines.size() : 0;
	if (count != cache.rows.size()) {
		cache.dirtyFrom = std::min(cache.dirtyFrom, std::min(count, cache.rows.size()));
		cache.naturalDirtyFrom = std::min(cache.naturalDirtyFrom, std::min(count, cache.rows.size()));
		cache.rows.resize(count);
	}
	for (size_t i = 0; i < count; ++i) {
		const std::string_view source =
		    lines[i].is_string() ? std::string_view(lines[i].get_ref<const std::string&>()) : std::string_view{};
		auto& row = cache.rows[i];
		if (source == row.source)
			continue;
		row.source.assign(source);
		AnsiState state;
		parseLine(row.source, 0, state, row.line);
		cache.dirtyFrom = std::min(cache.dirtyFrom, i);
		cache.naturalDirtyFrom = std::min(cache.naturalDirtyFrom, i);
	}
}

SDL_FPoint measureRows(const tsp::Node&, RowsCache& cache, text::Engine& engine, const text::Fonts& fonts, float width,
                       bool expanded) {
	cache.preferredWidth = width;
	cache.expanded = expanded;
	float available = std::max(0.0f, width);
	if (width >= naturalLimit) {
		for (size_t i = cache.naturalDirtyFrom; i < cache.rows.size(); ++i)
			cache.rows[i].line.widest =
			    std::max(i == 0 ? 0 : cache.rows[i - 1].line.widest, naturalWidth(cache.rows[i].line, fonts));
		cache.naturalDirtyFrom = cache.rows.size();
		available = cache.rows.empty() ? 0 : cache.rows.back().line.widest;
	}
	if (cache.contentWidth != available)
		cache.dirtyFrom = 0;
	cache.contentWidth = available;
	for (size_t i = cache.dirtyFrom; i < cache.rows.size(); ++i) {
		auto& line = cache.rows[i].line;
		layoutLine(line, engine, available, text::Wrap::None, cache.lineHeight, cache.ascent);
		line.top = static_cast<float>(i) * cache.lineHeight;
	}
	cache.dirtyFrom = cache.rows.size();
	cache.measuredWidth = available;
	cache.measuredHeight = static_cast<float>(cache.rows.size()) * cache.lineHeight;
	return {cache.measuredWidth, cache.measuredHeight};
}

bool paintRows(const tsp::Node& node, RowsCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, bool expanded, view::HitTargets& hits) {
	measureRows(node, cache, engine, fonts, rect.w, expanded);
	gfx::ScopedClip clip(renderer, rect);
	if (clip.visible.w <= 0 || clip.visible.h <= 0 || cache.rows.empty())
		return false;
	const size_t first = static_cast<size_t>(std::max(0.0f, std::floor((clip.visible.y - rect.y) / cache.lineHeight)));
	for (size_t i = first; i < cache.rows.size(); ++i) {
		const auto& line = cache.rows[i].line;
		const float top = rect.y + line.top;
		if (top >= clip.visible.y + clip.visible.h)
			break;
		engine.draw(line.runs, line.layout, rect.x, top);
		links(line, rect.x, top, clip.visible, hits);
	}
	return false;
}

void forgetRows(RowsCache& cache, text::Engine& engine) {
	for (const auto& row : cache.rows)
		engine.forget(row.line.runs);
	cache = {};
}

} // namespace kinds
