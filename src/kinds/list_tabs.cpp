#include "list_tabs.h"

#include "inline.h"
#include "gfx/draw.h"
#include "gfx/icons.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace kinds {
namespace {

constexpr float naturalLimit = 1000000;

void forgetText(ListText& text, text::Engine& engine) {
	engine.forget(text.runs);
}

void oneLine(ListText& text, text::Engine& engine, float width = naturalLimit) {
	text.layout = engine.layout(text.runs, {.width = std::max(0.0f, width), .wrap = text::Wrap::None, .maxLines = 1});
	if (width == naturalLimit)
		text.naturalWidth = text.layout.width;
}

void append(text::Runs& target, text::Runs source) {
	target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

char lower(char value) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

} // namespace

bool listTabsKind(tsp::Kind kind) {
	return kind == tsp::Kind::List || kind == tsp::Kind::Item || kind == tsp::Kind::Tabs;
}

void forgetListTabs(ListTabsCache& cache, text::Engine& engine) {
	for (auto& text : {std::ref(cache.label), std::ref(cache.detail), std::ref(cache.value), std::ref(cache.icon),
	                   std::ref(cache.empty)})
		forgetText(text.get(), engine);
	for (auto& hint : cache.hints)
		forgetText(hint, engine);
	for (auto& tab : cache.tabs)
		forgetText(tab.label, engine);
}

void syncListTabs(const tsp::Node& node, ListTabsCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	forgetListTabs(cache, engine);
	cache = {};
	const auto foreground = node.flag("disabled") ? text::palette::dim : text::palette::fg;
	const float em = fonts.size(), pad = em * gfx::space::sm, gap = em * gfx::space::sm;
	const float pillPad = em * gfx::space::md;
	if (node.kind == tsp::Kind::Item) {
		cache.label.runs = text::toRuns(node.prop("label"), {.color = foreground});
		cache.detail.runs = text::toRuns(node.prop("detail"),
		                                 {.color = node.flag("disabled") ? text::palette::dim : text::palette::muted});
		cache.value.runs = text::toRuns(node.prop("value"), {.color = text::palette::muted});
		const auto glyph = gfx::iconGlyph(node.str("icon"));
		if (!glyph.empty())
			cache.icon.runs = {{glyph,
			                    {.font = text::FontId::Icons,
			                     .color = node.flag("disabled") ? text::palette::dim : text::palette::muted}}};
		if (node.prop("hint").is_array())
			for (const auto& key : node.prop("hint")) {
				if (!key.is_string())
					continue;
				ListText hint;
				hint.runs = {{keyCap(key.get_ref<const std::string&>()), {.color = text::palette::muted}}};
				oneLine(hint, engine);
				if (!cache.hints.empty())
					cache.hintsWidth += gap;
				cache.hintsWidth += hint.layout.width + pillPad * 2;
				cache.hints.push_back(std::move(hint));
			}
		oneLine(cache.label, engine);
		oneLine(cache.detail, engine);
		oneLine(cache.value, engine);
		oneLine(cache.icon, engine);
		cache.valueWidth = cache.value.layout.width;
		cache.naturalWidth = pad * 2 + cache.label.naturalWidth
		                   + (cache.detail.runs.empty() ? 0 : gap + cache.detail.naturalWidth)
		                   + (cache.value.runs.empty() ? 0 : gap + cache.value.naturalWidth)
		                   + (cache.hints.empty() ? 0 : gap + cache.hintsWidth)
		                   + (cache.icon.runs.empty() ? 0 : cache.icon.naturalWidth + gap);
	} else if (node.kind == tsp::Kind::Tabs) {
		if (node.prop("items").is_array())
			for (const auto& item : node.prop("items")) {
				TabText tab;
				tab.id = item.value("id", "");
				tab.active = tab.id == node.str("active");
				tab.label.runs = text::toRuns(item.contains("label") ? item["label"] : json(tab.id),
				                              {.font = tab.active ? text::FontId::Bold : text::FontId::Ui,
				                               .color = tab.active ? text::palette::fg : text::palette::muted});
				oneLine(tab.label, engine);
				tab.width = tab.naturalWidth = tab.label.layout.width + pillPad * 2;
				tab.x = cache.naturalWidth;
				cache.naturalWidth += tab.width;
				cache.tabs.push_back(std::move(tab));
			}
	} else {
		cache.empty.runs = text::toRuns(node.prop("empty"), {.color = text::palette::muted});
		oneLine(cache.empty, engine);
		cache.naturalWidth = cache.empty.runs.empty() ? 0 : cache.empty.layout.width + pad * 2;
	}
}

SDL_FPoint measureListTabs(const tsp::Node& node, ListTabsCache& cache, text::Engine& engine, const text::Fonts& fonts,
                           float width) {
	width = std::max(0.0f, width);
	if (cache.width != width) {
		cache.width = width;
		const float em = fonts.size(), pad = em * gfx::space::sm, gap = em * gfx::space::sm;
		const float pillPad = em * gfx::space::md, pillTop = em * gfx::space::xs;
		if (node.kind == tsp::Kind::Item) {
			const float inset = std::min(pad, width * 0.5f), end = width - inset;
			cache.left = inset;
			cache.right = end;
			if (!cache.icon.runs.empty()) {
				oneLine(cache.icon, engine, std::min(cache.icon.naturalWidth, end - cache.left));
				cache.left = std::min(end, cache.left + cache.icon.layout.width + gap);
			}
			float naturalHints = 0;
			for (const auto& hint : cache.hints)
				naturalHints += hint.naturalWidth + pillPad * 2;
			if (!cache.hints.empty())
				naturalHints += gap * static_cast<float>(cache.hints.size() - 1);
			const float hintScale = naturalHints > 0 ? std::min(1.0f, (end - cache.left) / naturalHints) : 1;
			cache.hintsWidth = 0;
			cache.hintGap = gap * hintScale;
			for (auto& hint : cache.hints) {
				hint.width = (hint.naturalWidth + pillPad * 2) * hintScale;
				oneLine(hint, engine, hint.width - std::min(pillPad, hint.width * 0.5f) * 2);
				cache.hintsWidth += hint.width;
			}
			if (!cache.hints.empty()) {
				cache.hintsWidth += cache.hintGap * static_cast<float>(cache.hints.size() - 1);
				cache.right = std::max(cache.left, end - cache.hintsWidth - gap);
			}
			cache.hintsX = end - cache.hintsWidth;
			const float valueWidth = std::min({cache.valueWidth, width * 0.4f, cache.right - cache.left});
			oneLine(cache.value, engine, valueWidth);
			cache.valueX = cache.right - valueWidth;
			if (!cache.value.runs.empty())
				cache.right = std::max(cache.left, cache.valueX - gap);
			oneLine(cache.label, engine, cache.right - cache.left);
			cache.detailX = std::min(cache.right, cache.left + cache.label.layout.width + gap);
			oneLine(cache.detail, engine, cache.right - cache.detailX);
			float contentHeight = std::max({fonts.lineHeight(), cache.label.layout.height, cache.detail.layout.height,
			                                cache.value.layout.height, cache.icon.layout.height});
			for (const auto& hint : cache.hints)
				contentHeight = std::max(contentHeight, hint.layout.height + pillTop * 2);
			cache.height = contentHeight + pad * 2;
		} else if (node.kind == tsp::Kind::Tabs) {
			// Keep every tab clickable at narrow widths without letting its label overlap its neighbour.
			const float shrink = cache.naturalWidth > width && cache.naturalWidth > 0 ? width / cache.naturalWidth : 1;
			float x = 0, contentHeight = fonts.lineHeight();
			for (auto& tab : cache.tabs) {
				tab.width = tab.naturalWidth * shrink;
				tab.x = x;
				oneLine(tab.label, engine, tab.width - std::min(pillPad, tab.width * 0.5f) * 2);
				contentHeight = std::max(contentHeight, tab.label.layout.height);
				x += tab.width;
			}
			cache.height = contentHeight + pad * 2 + em * (2.0f / 15);
		} else {
			const float inset = std::min(pad, width * 0.5f);
			cache.empty.layout = engine.layout(cache.empty.runs, {.width = width - inset * 2});
			cache.height = cache.empty.runs.empty() ? 0 : cache.empty.layout.height + pad * 2;
		}
	}
	return {std::min(width, cache.naturalWidth), cache.height};
}

bool paintListTabs(const tsp::Node& node, ListTabsCache& cache, text::Engine& engine, const text::Fonts& fonts,
                   SDL_Renderer& renderer, const SDL_FRect& rect, view::HitTargets& hits) {
	measureListTabs(node, cache, engine, fonts, rect.w);
	const float em = fonts.size(), unit = em / 15, pad = em * gfx::space::sm;
	const float pillPad = em * gfx::space::md, pillTop = em * gfx::space::xs;
	if (node.kind == tsp::Kind::Item) {
		const float inset = std::min(pad, rect.w * 0.5f);
		engine.draw(cache.icon.runs, cache.icon.layout, rect.x + inset,
		            rect.y + (cache.height - cache.icon.layout.height) * 0.5f);
		engine.draw(cache.label.runs, cache.label.layout, rect.x + cache.left,
		            rect.y + (cache.height - cache.label.layout.height) * 0.5f);
		engine.draw(cache.detail.runs, cache.detail.layout, rect.x + cache.detailX,
		            rect.y + (cache.height - cache.detail.layout.height) * 0.5f);
		engine.draw(cache.value.runs, cache.value.layout, rect.x + cache.valueX,
		            rect.y + (cache.height - cache.value.layout.height) * 0.5f);
		float x = rect.x + cache.hintsX;
		for (const auto& hint : cache.hints) {
			const float height = hint.layout.height + pillTop * 2;
			const SDL_FRect key{x, rect.y + (cache.height - height) * 0.5f, hint.width, height};
			gfx::rounded(renderer, key, em * 0.22f, text::palette::surface2);
			gfx::roundedBorder(renderer, key, em * 0.22f, text::palette::border, unit);
			engine.draw(hint.runs, hint.layout, x + std::min(pillPad, hint.width * 0.5f), key.y + pillTop);
			x += hint.width + cache.hintGap;
		}
		if (!node.flag("disabled"))
			hits.add(rect, view::TargetKind::Select, node.id, node.parent);
	} else if (node.kind == tsp::Kind::Tabs) {
		gfx::line(renderer, {rect.x, rect.y + rect.h - unit}, {rect.x + rect.w, rect.y + rect.h - unit},
		          text::palette::border, unit);
		for (const auto& tab : cache.tabs) {
			const SDL_FRect hit{rect.x + tab.x, rect.y, tab.width, rect.h};
			engine.draw(tab.label.runs, tab.label.layout, hit.x + std::min(pillPad, tab.width * 0.5f), rect.y + pad);
			if (tab.active)
				gfx::fill(renderer, {hit.x, rect.y + rect.h - 2 * unit, hit.w, 2 * unit}, text::palette::accent);
			hits.add(hit, view::TargetKind::Select, tab.id);
		}
	} else
		engine.draw(cache.empty.runs, cache.empty.layout, rect.x + std::min(pad, rect.w * 0.5f), rect.y + pad);
	return false;
}

bool listItemMatches(const tsp::Node& node, std::string_view filter) {
	if (filter.empty())
		return true;
	const auto label = text::plainText(node.prop("label"));
	return std::search(label.begin(), label.end(), filter.begin(), filter.end(),
	                   [](char a, char b) { return lower(a) == lower(b); })
	    != label.end();
}

} // namespace kinds
