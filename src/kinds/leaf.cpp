#include "leaf.h"

#include "core/text_util.h"
#include "gfx/draw.h"
#include "gfx/icons.h"
#include "chrome.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <utility>

namespace kinds {
namespace {

constexpr float naturalLimit = 1000000;
constexpr float pi = std::numbers::pi_v<float>;

bool editor(tsp::Kind kind) {
	return kind == tsp::Kind::Editor || kind == tsp::Kind::Input;
}

text::RunStyle style(SDL_Color color, text::FontId font = text::FontId::Ui) {
	text::RunStyle result;
	result.color = color;
	result.font = font;
	return result;
}

SDL_Color tone(std::string_view name, SDL_Color fallback) {
	auto result = style(fallback);
	text::applyTokens(name, result);
	return result.color;
}

void append(text::Runs& target, text::Runs source) {
	target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

text::Runs content(const tsp::Node& node, const text::RunStyle& base) {
	const auto& spans = node.prop("spans");
	return text::toRuns(spans.is_null() ? node.prop("text") : spans, base);
}

text::Layout singleLine(text::Engine& engine, const text::Runs& runs, float width = naturalLimit) {
	return engine.layout(runs, {.width = width, .wrap = text::Wrap::None, .maxLines = 1});
}

float modeWidth(const tsp::Node& node, const LeafCache& cache, const text::Fonts& fonts) {
	return editor(node.kind) && !cache.label.empty()
	         ? cache.labelLayout.width + fonts.size() * (gfx::space::md * 2 + gfx::space::sm)
	         : 0;
}

float layoutLineHeight(const text::Layout& layout, const text::Fonts& fonts) {
	return layout.lineHeight > 0 ? layout.lineHeight : fonts.lineHeight();
}

// A TspExtent cap: `Nch`, `Nlines`, or a fraction of `whole` (which is also the default).
float extent(const json& value, float whole, const text::Fonts& fonts) {
	if (value.is_number())
		return whole * value.get<float>();
	if (!value.is_string())
		return whole;
	const auto& textValue = value.get_ref<const std::string&>();
	const float count = std::strtof(textValue.c_str(), nullptr);
	if (textValue.ends_with("ch"))
		return std::min(whole, count * fonts.monoAdvance());
	if (textValue.ends_with("lines"))
		return count * fonts.lineHeight();
	return whole;
}

SDL_FPoint size(const tsp::Node& node, const LeafCache& cache, const text::Fonts& fonts) {
	float width = std::max(cache.layout.width, node.kind == tsp::Kind::Toast ? cache.labelLayout.width : 0.0f);
	float height = cache.layout.height;
	if (editor(node.kind)) {
		width = std::min(width + std::max(1.0f, fonts.size() * (2.0f / 15)), cache.options.width);
		if (cache.options.maxLines > 0)
			height =
			    std::min(height, static_cast<float>(cache.options.maxLines) * layoutLineHeight(cache.layout, fonts));
	}
	if (editor(node.kind) && !cache.label.empty())
		height = std::max(height, cache.labelLayout.height + fonts.size() * gfx::space::xs * 2);
	return {width + cache.padX * 2 + modeWidth(node, cache, fonts),
	        height + cache.padY * 2
	            + (node.kind == tsp::Kind::Toast && !cache.label.empty()
	                   ? cache.labelLayout.height + fonts.size() * gfx::space::sm
	                   : 0)};
}

bool shimmerVisible(const text::Runs& runs, const text::Layout& layout, const text::Fonts& fonts, float x, float y,
                    const SDL_FRect& visible) {
	const float lineHeight = layoutLineHeight(layout, fonts);
	for (const auto& piece : layout.pieces) {
		if (runs[piece.run].style.fx != 0 && piece.width > 0
		    && gfx::intersects({x + piece.x, y + static_cast<float>(piece.line) * lineHeight, piece.width, lineHeight},
		                       visible))
			return true;
	}
	return false;
}

void logo(SDL_Renderer& renderer, const SDL_FRect& rect) {
	const float side = std::min(rect.w, rect.h);
	gfx::gradientRounded(renderer, {rect.x, rect.y, side, side}, side * 0.18f,
	                     {text::palette::accent, text::palette::pending, text::palette::error, text::palette::info});
	// A scalable pi keeps the mark crisp without changing the shared font size or renderer scale.
	const float stroke = side * 0.068f;
	auto point = [&](float x, float y) {
		return SDL_FPoint{rect.x + side * x, rect.y + side * y};
	};
	gfx::line(renderer, point(0.23f, 0.37f), point(0.31f, 0.32f), text::palette::bg, stroke);
	gfx::line(renderer, point(0.31f, 0.32f), point(0.77f, 0.32f), text::palette::bg, stroke);
	gfx::line(renderer, point(0.41f, 0.33f), point(0.36f, 0.72f), text::palette::bg, stroke);
	gfx::line(renderer, point(0.64f, 0.33f), point(0.64f, 0.66f), text::palette::bg, stroke);
	gfx::arc(renderer, point(0.69f, 0.66f), side * 0.05f, pi * 0.1f, pi, text::palette::bg, stroke);
}

void welcomeLogo(SDL_Renderer& renderer, const SDL_FRect& rect) {
	const float side = std::min(rect.w, rect.h);
	const auto white = text::palette::mark;
	gfx::fill(renderer, {rect.x + side * 0.05f, rect.y + side * 0.1f, side * 0.9f, side * 0.16f}, white);
	gfx::fill(renderer, {rect.x + side * 0.29f, rect.y + side * 0.24f, side * 0.16f, side * 0.44f}, white);
	gfx::fill(renderer, {rect.x + side * 0.61f, rect.y + side * 0.24f, side * 0.16f, side * 0.6f}, white);
}

} // namespace

bool measured(tsp::Kind kind) {
	if (dataKind(kind) || inlineKind(kind) || listTabsKind(kind))
		return true;
	switch (kind) {
	case tsp::Kind::Rule:
	case tsp::Kind::Text:
	case tsp::Kind::Shimmer:
	case tsp::Kind::Image:
	case tsp::Kind::Seg:
	case tsp::Kind::Toast:
	case tsp::Kind::Editor:
	case tsp::Kind::Input:
	case tsp::Kind::Math:
	case tsp::Kind::Media:
	case tsp::Kind::Md:
	case tsp::Kind::Code:
	case tsp::Kind::Diff:
	case tsp::Kind::Ansi:
	case tsp::Kind::Rows:
	case tsp::Kind::Tool:
	case tsp::Kind::Checklist:
	case tsp::Kind::Agent:
		return true;
	default:
		return false;
	}
}

void sync(const tsp::Node& node, LeafCache& cache, text::Engine& engine, const text::Fonts& fonts, bool chromeless) {
	if (dataKind(node.kind)) {
		if (!cache.data)
			cache.data = std::make_unique<DataCache>();
		syncData(node, *cache.data, engine, fonts);
		return;
	}
	if (inlineKind(node.kind)) {
		if (!cache.inlined)
			cache.inlined = std::make_unique<InlineCache>();
		syncInline(node, *cache.inlined, engine, fonts);
		return;
	}
	if (listTabsKind(node.kind)) {
		if (!cache.listTabs)
			cache.listTabs = std::make_unique<ListTabsCache>();
		syncListTabs(node, *cache.listTabs, engine, fonts);
		return;
	}
	switch (node.kind) {
	case tsp::Kind::Prefs:
		if (!cache.prefs)
			cache.prefs = std::make_unique<PrefsCache>();
		syncPrefs(node, *cache.prefs, engine);
		return;
	case tsp::Kind::Picker:
		if (!cache.picker)
			cache.picker = std::make_unique<PickerCache>();
		syncPicker(node, *cache.picker, engine, fonts);
		return;
	case tsp::Kind::Tool:
	case tsp::Kind::Checklist:
	case tsp::Kind::Agent:
		if (!cache.toolAgent)
			cache.toolAgent = std::make_unique<ToolAgentCache>();
		cache.toolAgent->chromeless = chromeless;
		syncToolAgent(node, *cache.toolAgent, engine, fonts);
		return;
	case tsp::Kind::Md:
		if (!cache.md)
			cache.md = std::make_unique<MdCache>();
		syncMd(node, *cache.md, engine, fonts);
		return;
	case tsp::Kind::Code:
	case tsp::Kind::Diff:
		if (!cache.code)
			cache.code = std::make_unique<CodeCache>();
		syncCode(node, *cache.code, engine, fonts, chromeless);
		return;
	case tsp::Kind::Ansi:
		if (!cache.ansi)
			cache.ansi = std::make_unique<AnsiCache>();
		syncAnsi(node, *cache.ansi, engine, fonts, chromeless);
		return;
	case tsp::Kind::Rows:
		if (!cache.rows)
			cache.rows = std::make_unique<RowsCache>();
		syncRows(node, *cache.rows, engine, fonts, chromeless);
		return;
	default:
		break;
	}
	const float scrollX = cache.textScrollX, scrollY = cache.textScrollY;
	auto preedit = std::move(cache.preedit);
	const bool expanded = cache.expanded;
	const auto editorSelection = cache.editorSelection;
	auto image = std::move(cache.image);
	forget(cache, engine);
	cache.preedit = std::move(preedit);
	cache.expanded = expanded;
	cache.editorSelection = editorSelection;
	cache.image = std::move(image);
	const float unit = fonts.size() / 15;
	auto base = style(tone(node.str("tone"), text::palette::fg));
	if (node.str("role") == "omp.welcome.wordmark") {
		base.font = text::FontId::Bold;
	} else if (node.str("role") == "omp.welcome.version")
		base = style(text::palette::dim, text::FontId::Mono);
	const auto wrap = node.str("wrap", "word"), truncate = node.str("truncate", "end");
	cache.options.width = naturalLimit;
	cache.options.wrap = wrap == "none" ? text::Wrap::None : wrap == "char" ? text::Wrap::Char : text::Wrap::Word;
	cache.options.truncate = truncate == "start"  ? text::Truncate::Start
	                       : truncate == "middle" ? text::Truncate::Middle
	                                              : text::Truncate::End;
	// An explicit `truncate` without `lines` asks for one line cut where it says (a path, a branch).
	cache.options.maxLines = node.prop("lines").is_number()    ? static_cast<int>(node.num("lines"))
	                       : node.prop("truncate").is_string() ? 1
	                                                           : 0;

	if (editor(node.kind)) {
		cache.textScrollX = scrollX;
		cache.textScrollY = scrollY;
		cache.padX = chromeless ? 0 : fonts.size() * gfx::space::md;
		cache.padY = chromeless ? 0 : fonts.size() * gfx::space::sm;
		if (!node.str("lang").empty())
			base.font = text::FontId::Mono;
		cache.options.caret = editorRuns(node, cache.runs, base, cache.editorSpans, cache.editorSelection);
		cache.label = text::toRuns(node.prop("mode"), style(text::palette::pending));
		if (!cache.label.empty())
			cache.labelLayout = singleLine(engine, cache.label);
		cache.options.maxLines = node.kind == tsp::Kind::Input ? 1 : static_cast<int>(node.num("maxLines"));
		if (node.kind == tsp::Kind::Input)
			cache.options.wrap = text::Wrap::None;
	} else if (node.kind == tsp::Kind::Image && node.str("builtin") == "omp") {
		cache.naturalWidth = node.str("role") == "omp.welcome.logo"
		                       ? unit * 48
		                       : std::max(fonts.lineHeight() * 2, static_cast<float>(node.num("w", 64)) * 0.5f * unit);
		cache.layout.width = cache.layout.height = cache.naturalWidth;
		return;
	} else if (node.kind == tsp::Kind::Image && cache.image) {
		cache.naturalWidth = cache.image->fitted(unit, naturalLimit, naturalLimit).x;
		return;
	} else if (node.kind == tsp::Kind::Rule) {
		cache.label = text::toRuns(node.prop("label"), style(text::palette::muted));
		if (!cache.label.empty())
			cache.labelLayout = singleLine(engine, cache.label);
		cache.naturalWidth = std::max(fonts.size() * 6, cache.labelLayout.width + fonts.size());
		cache.layout.width = cache.naturalWidth;
		cache.layout.height = fonts.lineHeight();
		return;
	} else if (node.kind == tsp::Kind::Seg) {
		base.color = tone(node.str("tone"), text::palette::muted);
		if (auto glyph = gfx::iconGlyph(node.str("icon")); !glyph.empty()) {
			cache.runs.push_back({std::move(glyph), style(base.color, text::FontId::Icons)});
			cache.runs.push_back({" ", base});
		}
		append(cache.runs, content(node, base));
		cache.options.wrap = text::Wrap::None;
	} else if (node.kind == tsp::Kind::Toast) {
		cache.padX = fonts.size() * gfx::space::lg;
		cache.padY = fonts.size() * gfx::space::md;
		cache.runs = content(node, base);
		cache.label = text::toRuns(node.prop("sub"), style(text::palette::muted));
		if (!cache.label.empty())
			cache.labelLayout = engine.layout(cache.label, cache.options);
	} else if (node.kind == tsp::Kind::Image || node.kind == tsp::Kind::Media) {
		cache.runs.push_back({gfx::iconGlyph("image"), style(text::palette::muted, text::FontId::Icons)});
		cache.runs.push_back(
		    {" " + std::string(node.str("alt", node.str("path", "image"))), style(text::palette::muted)});
	} else {
		if (node.kind == tsp::Kind::Math)
			base.font = text::FontId::Mono;
		cache.runs = content(node, base);
	}
	if (node.str("role") == "omp.welcome.wordmark" || node.str("role") == "omp.welcome.version") {
		cache.options.wrap = text::Wrap::None;
		const auto font = node.str("role") == "omp.welcome.wordmark" ? text::FontId::Wordmark : text::FontId::Mono;
		for (auto& run : cache.runs) {
			run.style.font = font;
			run.style.icon = false;
		}
	}
	if (node.kind == tsp::Kind::Shimmer)
		for (auto& run : cache.runs)
			run.style.fx = 1;
	auto options = cache.options;
	if (editor(node.kind))
		options.maxLines = 0;
	cache.layout = engine.layout(cache.runs, options);
	cache.naturalWidth = std::max(cache.layout.width, node.kind == tsp::Kind::Toast ? cache.labelLayout.width : 0.0f)
	                   + cache.padX * 2 + modeWidth(node, cache, fonts);
	if (editor(node.kind))
		cache.naturalWidth += std::max(1.0f, unit * 2);
}

SDL_FPoint measure(const tsp::Node& node, LeafCache& cache, text::Engine& engine, const text::Fonts& fonts,
                   float width) {
	if (dataKind(node.kind))
		return measureData(node, *cache.data, engine, fonts, width);
	if (inlineKind(node.kind))
		return measureInline(node, *cache.inlined, engine, fonts, width);
	if (listTabsKind(node.kind))
		return measureListTabs(node, *cache.listTabs, engine, fonts, width);
	switch (node.kind) {
	case tsp::Kind::Prefs:
		return measurePrefs(node, *cache.prefs, engine, fonts, width);
	case tsp::Kind::Picker:
		return {width, fonts.lineHeight() * 22};
	case tsp::Kind::Tool:
	case tsp::Kind::Checklist:
	case tsp::Kind::Agent:
		return measureToolAgent(node, *cache.toolAgent, engine, fonts, width);
	case tsp::Kind::Md:
		return measureMd(*cache.md, engine, fonts, width);
	case tsp::Kind::Code:
	case tsp::Kind::Diff:
		cache.code->budget = cache.budget;
		return measureCode(node, *cache.code, engine, fonts, width, cache.expanded);
	case tsp::Kind::Ansi:
		return measureAnsi(node, *cache.ansi, engine, fonts, width, cache.expanded);
	case tsp::Kind::Rows:
		return measureRows(node, *cache.rows, engine, fonts, width, cache.expanded);
	default:
		break;
	}
	width = std::max(0.0f, width);
	if (cache.width == width)
		return size(node, cache, fonts);
	cache.width = width;
	const float available = width >= naturalLimit ? cache.naturalWidth : width;
	if (node.kind == tsp::Kind::Image && node.str("builtin") == "omp") {
		cache.layout.width = cache.layout.height = std::min(available, cache.naturalWidth);
	} else if (node.kind == tsp::Kind::Image && cache.image) {
		const auto& max = node.prop("max");
		const auto size = cache.image->fitted(
		    fonts.size() / 15, extent(max.is_object() ? max.value("w", json()) : json(), available, fonts),
		    extent(max.is_object() ? max.value("h", json()) : json(), fonts.lineHeight() * gfx::pictureLines, fonts));
		cache.layout.width = size.x;
		cache.layout.height = size.y;
	} else if (node.kind == tsp::Kind::Rule) {
		cache.layout.width = available;
		if (!cache.label.empty())
			cache.labelLayout = singleLine(engine, cache.label, available);
	} else {
		cache.options.width = std::max(0.0f, available - cache.padX * 2 - modeWidth(node, cache, fonts));
		if (editor(node.kind)) {
			const float caretWidth = std::max(1.0f, fonts.size() * (2.0f / 15));
			auto options = cache.options;
			options.maxLines = 0;
			options.width =
			    options.wrap == text::Wrap::None ? naturalLimit : std::max(0.0f, options.width - caretWidth);
			cache.layout = engine.layout(cache.runs, options);
			const float lineHeight = layoutLineHeight(cache.layout, fonts);
			const float height =
			    cache.options.maxLines > 0
			        ? std::min(cache.layout.height, static_cast<float>(cache.options.maxLines) * lineHeight)
			        : cache.layout.height;
			const float maxY = std::max(0.0f, cache.layout.height - height);
			const float maxX = options.wrap == text::Wrap::None
			                     ? std::max(0.0f, cache.layout.width + caretWidth - cache.options.width)
			                     : 0;
			cache.textScrollX = std::clamp(cache.textScrollX, 0.0f, maxX);
			cache.textScrollY = std::clamp(cache.textScrollY, 0.0f, maxY);
			if (cache.layout.caret) {
				const auto caret = *cache.layout.caret;
				cache.textScrollY = std::clamp(std::min(cache.textScrollY, caret.y), 0.0f, maxY);
				if (caret.y + lineHeight > cache.textScrollY + height)
					cache.textScrollY = std::min(maxY, caret.y + lineHeight - height);
				if (options.wrap == text::Wrap::None) {
					cache.textScrollX = std::min(cache.textScrollX, caret.x);
					if (caret.x + caretWidth > cache.textScrollX + cache.options.width)
						cache.textScrollX = std::min(maxX, caret.x + caretWidth - cache.options.width);
				}
			}
		} else
			cache.layout = engine.layout(cache.runs, cache.options);
		if (node.kind == tsp::Kind::Toast && !cache.label.empty())
			cache.labelLayout = engine.layout(cache.label, cache.options);
	}
	return size(node, cache, fonts);
}

LeafPaint paint(const tsp::Node& node, LeafCache& cache, text::Engine& engine, const text::Fonts& fonts,
                SDL_Renderer& renderer, const SDL_FRect& rect, bool focused, view::HitTargets& hits) {
	if (dataKind(node.kind))
		return {.animating = paintData(node, *cache.data, engine, fonts, renderer, rect, hits)};
	if (inlineKind(node.kind)) {
		paintInline(node, *cache.inlined, engine, fonts, renderer, rect, hits);
		const bool resized = std::exchange(cache.inlined->sizeChanged, false);
		return {.nextWake = cache.inlined->nextWake, .sizeChanged = resized};
	}
	if (listTabsKind(node.kind))
		return {.animating = paintListTabs(node, *cache.listTabs, engine, fonts, renderer, rect, hits)};
	if (node.kind == tsp::Kind::Prefs)
		return {.caret = paintPrefs(node, *cache.prefs, engine, fonts, renderer, rect, focused, hits)};
	if (node.kind == tsp::Kind::Picker) {
		const bool animating = paintPicker(node, *cache.picker, engine, fonts, renderer, rect, focused, hits);
		return {.caret = cache.picker->caret,
		        .animating = animating,
		        .nextWake =
		            cache.picker->nextWake >= 0 ? SDL_GetTicks() + static_cast<Uint64>(cache.picker->nextWake) : 0};
	}
	measure(node, cache, engine, fonts, rect.w);
	LeafPaint result;
	const float unit = fonts.size() / 15;
	const float lineHeight = editor(node.kind) ? layoutLineHeight(cache.layout, fonts) : fonts.lineHeight();
	float x = rect.x + cache.padX, y = rect.y + cache.padY;
	if (editor(node.kind)) {
		x -= cache.textScrollX;
		y -= cache.textScrollY;
	}
	if (editor(node.kind) && cache.layout.caret)
		result.caret = SDL_FRect{x + cache.layout.caret->x, y + cache.layout.caret->y + unit * 2,
		                         std::max(1.0f, unit * 2), std::max(1.0f, lineHeight - unit * 4)};
	gfx::ScopedClip clip(renderer, rect);
	if (clip.visible.w <= 0 || clip.visible.h <= 0)
		return result;
	switch (node.kind) {
	case tsp::Kind::Tool:
	case tsp::Kind::Checklist:
	case tsp::Kind::Agent:
		return {.animating =
		            paintToolAgent(node, *cache.toolAgent, engine, fonts, renderer, rect, cache.collapsed, hits)};
	case tsp::Kind::Md:
		return {.animating = paintMd(*cache.md, engine, fonts, renderer, rect, hits)};
	case tsp::Kind::Code:
	case tsp::Kind::Diff:
		cache.code->budget = cache.budget;
		return {.animating = paintCode(node, *cache.code, engine, fonts, renderer, rect, cache.expanded, hits)};
	case tsp::Kind::Ansi:
		return {.animating = paintAnsi(node, *cache.ansi, engine, fonts, renderer, rect, cache.expanded, hits)};
	case tsp::Kind::Rows:
		return {.animating = paintRows(node, *cache.rows, engine, fonts, renderer, rect, cache.expanded, hits)};
	default:
		break;
	}
	if (node.kind == tsp::Kind::Image && node.str("builtin") == "omp") {
		const SDL_FRect mark{rect.x, rect.y, cache.layout.width, cache.layout.height};
		if (node.str("role") == "omp.welcome.logo")
			welcomeLogo(renderer, mark);
		else
			logo(renderer, mark);
	} else if (node.kind == tsp::Kind::Image && cache.image) {
		engine.drawImage(*cache.image, {rect.x, rect.y, cache.layout.width, cache.layout.height});
	} else if (node.kind == tsp::Kind::Rule) {
		const float middle = rect.y + lineHeight * 0.5f;
		if (cache.label.empty())
			gfx::line(renderer, {rect.x, middle}, {rect.x + rect.w, middle}, text::palette::border, unit);
		else {
			const float left = rect.x + (rect.w - cache.labelLayout.width) * 0.5f;
			if (left - fonts.size() * 0.5f > rect.x)
				gfx::line(renderer, {rect.x, middle}, {left - fonts.size() * 0.5f, middle}, text::palette::border,
				          unit);
			if (left + cache.labelLayout.width + fonts.size() * 0.5f < rect.x + rect.w)
				gfx::line(renderer, {left + cache.labelLayout.width + fonts.size() * 0.5f, middle},
				          {rect.x + rect.w, middle}, text::palette::border, unit);
			engine.draw(cache.label, cache.labelLayout, left, rect.y);
			result.animating = shimmerVisible(cache.label, cache.labelLayout, fonts, left, rect.y, clip.visible);
		}
	} else if (node.str("role") == "omp.composer.stop") {
		// Takes the send button's place while a turn runs: the same circle, holding a stop square.
		const float side = std::min(rect.w, rect.h), square = side * 0.3f;
		const SDL_FRect button{rect.x + (rect.w - side) * 0.5f, rect.y + (rect.h - side) * 0.5f, side, side};
		gfx::rounded(renderer, button, side * 0.5f, text::withAlpha(text::palette::error, 40));
		gfx::roundedBorder(renderer, button, side * 0.5f, text::withAlpha(text::palette::error, 140), unit);
		gfx::rounded(renderer, {button.x + (side - square) * 0.5f, button.y + (side - square) * 0.5f, square, square},
		             unit * 2, text::palette::error);
	} else {
		if (node.kind == tsp::Kind::Toast) {
			auto fill = text::palette::surface2, border = tone(node.str("tone"), text::palette::border);
			fill.a = static_cast<Uint8>(std::lround(static_cast<float>(fill.a) * cache.opacity));
			border.a = static_cast<Uint8>(std::lround(static_cast<float>(border.a) * cache.opacity));
			gfx::rounded(renderer, rect, unit * 8, fill);
			gfx::roundedBorder(renderer, rect, unit * 8, border, unit);
		} else if (editor(node.kind) && cache.padX > 0) {
			gfx::roundedBorder(renderer, rect, unit * 6, focused ? text::palette::accent : text::palette::border, unit);
		}
		if (editor(node.kind)) {
			const SDL_FRect viewport{rect.x + cache.padX, rect.y + cache.padY, cache.options.width,
			                         std::max(0.0f, rect.h - cache.padY * 2)};
			{
				gfx::ScopedClip textClip(renderer, viewport);
				engine.draw(cache.runs, cache.layout, x, y);
				cache.textOrigin = {x, y};
				result.animating = shimmerVisible(cache.runs, cache.layout, fonts, x, y, textClip.visible);
				if (focused && !node.flag("readonly") && result.caret) {
					// Blinks after it moves, then rests solid so an idle prompt stops repainting.
					constexpr Uint64 half = 530, blinking = 10000;
					const Uint64 now = SDL_GetTicks();
					const auto& caret = *result.caret;
					if (caret.x != cache.caretRect.x || caret.y != cache.caretRect.y || caret.h != cache.caretRect.h) {
						cache.caretRect = caret;
						cache.caretSince = now;
					}
					const Uint64 age = now - cache.caretSince;
					if (age >= blinking || (age / half) % 2 == 0)
						gfx::fill(renderer, caret, text::palette::accent);
					if (age < blinking)
						result.nextWake = cache.caretSince + (age / half + 1) * half;
				}
			}
			if (focused && result.caret)
				paintPreedit(cache.preedit, engine, fonts, renderer, *result.caret);
		} else {
			engine.draw(cache.runs, cache.layout, x, y, cache.opacity);
			result.animating = shimmerVisible(cache.runs, cache.layout, fonts, x, y, clip.visible);
		}
		if (node.kind == tsp::Kind::Toast && !cache.label.empty()) {
			const float subY = y + cache.layout.height + fonts.size() * gfx::space::sm;
			engine.draw(cache.label, cache.labelLayout, x, subY, cache.opacity);
			result.animating |= shimmerVisible(cache.label, cache.labelLayout, fonts, x, subY, clip.visible);
		}
		if (editor(node.kind)) {
			if (!cache.label.empty()) {
				const float width = modeWidth(node, cache, fonts) - fonts.size() * gfx::space::sm;
				const float left = rect.x + rect.w - cache.padX - width;
				const float height = cache.labelLayout.height + fonts.size() * gfx::space::xs * 2;
				const float top = rect.y + (rect.h - height) * 0.5f;
				auto color = text::palette::pending;
				color.a = 45;
				gfx::rounded(renderer, {left, top, width, height}, height * 0.5f, color);
				engine.draw(cache.label, cache.labelLayout, left + fonts.size() * gfx::space::md,
				            top + fonts.size() * gfx::space::xs);
			}
		}
		for (const auto& piece : cache.layout.pieces) {
			const auto& href = cache.runs[piece.run].style.href;
			if (!href.empty())
				hits.add({x + piece.x, y + piece.line * lineHeight, piece.width, lineHeight}, view::TargetKind::Link,
				         href);
		}
	}
	return result;
}

void forget(LeafCache& cache, text::Engine& engine) {
	if (cache.md)
		forgetMd(*cache.md, engine);
	if (cache.code)
		forgetCode(*cache.code, engine);
	if (cache.ansi)
		forgetAnsi(*cache.ansi, engine);
	if (cache.rows)
		forgetRows(*cache.rows, engine);
	if (cache.toolAgent)
		forgetToolAgent(*cache.toolAgent, engine);
	if (cache.data)
		forgetData(*cache.data, engine);
	if (cache.inlined)
		forgetInline(*cache.inlined, engine);
	if (cache.listTabs)
		forgetListTabs(*cache.listTabs, engine);
	if (cache.prefs)
		forgetPrefs(*cache.prefs, engine);
	if (cache.picker)
		forgetPicker(*cache.picker, engine);
	if (!cache.runs.empty())
		engine.forget(cache.runs);
	if (!cache.label.empty())
		engine.forget(cache.label);
	engine.forget(cache.preedit.runs);
	cache = {};
}

namespace {

// Text as line segments: neighbouring pieces of one colour on a line merge into one bar, and
// blank pieces (indentation, spaces) leave gaps.
void textBars(const text::Runs& runs, const text::Layout& layout, float x, float y, float lineHeight,
              std::vector<OverviewBar>& out) {
	const size_t first = out.size();
	for (const auto& piece : layout.pieces) {
		if (piece.width <= 0 || piece.run >= runs.size())
			continue;
		const auto& run = runs[piece.run];
		const auto value = std::string_view(run.text).substr(piece.begin, piece.end - piece.begin);
		if (value.find_first_not_of(" \t\r\n") == std::string_view::npos)
			continue;
		const SDL_FRect rect{x + piece.x, y + static_cast<float>(piece.line) * lineHeight + lineHeight * 0.2f,
		                     piece.width, lineHeight * 0.6f};
		const auto color = run.style.color;
		if (out.size() > first) {
			auto& last = out.back();
			if (last.rect.y == rect.y && last.color.r == color.r && last.color.g == color.g && last.color.b == color.b
			    && last.color.a == color.a && rect.x - (last.rect.x + last.rect.w) < lineHeight * 0.5f
			    && rect.x >= last.rect.x) {
				last.rect.w = rect.x + rect.w - last.rect.x;
				continue;
			}
		}
		out.push_back({rect, color});
	}
}

} // namespace

void overview(const tsp::Node& node, const LeafCache& cache, const text::Fonts& fonts, SDL_FPoint size,
              std::vector<OverviewBar>& out) {
	const size_t first = out.size();
	if (cache.md) {
		const float pad = fonts.size() * gfx::space::lg, cellPadY = fonts.size() * gfx::space::sm;
		for (const auto& block : cache.md->blocks) {
			if (block.kind == MdBlock::Kind::Picture) {
				out.push_back({{0, block.y, block.layout.width, block.layout.height}, text::palette::border});
				continue;
			}
			if (block.kind == MdBlock::Kind::Table) {
				for (const auto& cell : block.cells)
					textBars(cell.runs, cell.layout, cell.x + pad, block.y + cell.y + cellPadY,
					         layoutLineHeight(cell.layout, fonts), out);
				continue;
			}
			if (block.kind == MdBlock::Kind::Code)
				out.push_back({{0, block.y, size.x, block.height}, text::palette::surface2});
			textBars(block.runs, block.layout, block.indent, block.y + (block.kind == MdBlock::Kind::Code ? pad : 0),
			         layoutLineHeight(block.layout, fonts), out);
		}
	} else if (cache.code) {
		const auto& code = *cache.code;
		const float top = code.headerHeight;
		out.push_back({{0, top, size.x, code.bodyHeight}, text::palette::surface2});
		for (size_t i = 0; i < std::min(code.visibleLines, code.laidLines) && i < code.lines.size(); ++i) {
			const auto& line = code.lines[i];
			if (line.background.a)
				out.push_back({{0, top + line.y, size.x, line.visibleHeight}, line.background});
			const bool fullWidth = line.kind == CodeCache::LineKind::Hunk || line.kind == CodeCache::LineKind::Meta;
			const auto& layout = line.visibleHeight < line.layout.height ? line.previewLayout : line.layout;
			textBars(line.runs, layout, code.padX + (fullWidth ? 0 : code.gutter), top + line.y,
			         layoutLineHeight(layout, fonts), out);
		}
	} else if (cache.ansi) {
		const auto& ansi = *cache.ansi;
		if (!ansi.chromeless)
			out.push_back({{0, 0, size.x, size.y}, text::palette::surface2});
		const float hidden =
		    ansi.hiddenLines > 0 && ansi.hiddenLines < ansi.lines.size() ? ansi.lines[ansi.hiddenLines].top : 0;
		const float lineHeight = ansi.lineHeight > 0 ? ansi.lineHeight : fonts.lineHeight();
		for (size_t i = ansi.hiddenLines; i < ansi.lines.size(); ++i) {
			const auto& line = ansi.lines[i];
			const float y = ansi.padY + line.top - hidden;
			if (y > size.y)
				break;
			textBars(line.runs, line.layout, ansi.padX, y, lineHeight, out);
		}
	} else if (cache.rows) {
		const float lineHeight = cache.rows->lineHeight > 0 ? cache.rows->lineHeight : fonts.lineHeight();
		for (const auto& row : cache.rows->rows) {
			if (row.line.top > size.y)
				break;
			textBars(row.line.runs, row.line.layout, 0, row.line.top, lineHeight, out);
		}
	} else if (cache.toolAgent) {
		eachShownRow(*cache.toolAgent, [&](const text::Runs& runs, const text::Layout& layout, const SDL_FRect& rect) {
			textBars(runs, layout, rect.x, rect.y, layoutLineHeight(layout, fonts), out);
		});
	} else if (node.kind == tsp::Kind::Image) {
		out.push_back({{0, 0, size.x, size.y}, text::palette::border});
	} else if (!cache.runs.empty())
		textBars(cache.runs, cache.layout, cache.padX, cache.padY, layoutLineHeight(cache.layout, fonts), out);
	// Keep to the leaf's box: collapsed previews and scrolled blocks show only part of their text.
	auto end = std::remove_if(out.begin() + static_cast<ptrdiff_t>(first), out.end(), [&](OverviewBar& bar) {
		const float top = std::max(0.0f, bar.rect.y), bottom = std::min(size.y, bar.rect.y + bar.rect.h);
		const float right = std::min(size.x, bar.rect.x + bar.rect.w);
		bar.rect = {bar.rect.x, top, right - bar.rect.x, bottom - top};
		return bar.rect.w <= 0 || bar.rect.h <= 0;
	});
	out.erase(end, out.end());
}

} // namespace kinds
