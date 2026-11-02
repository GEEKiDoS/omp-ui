#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"
#include "view/hit_targets.h"

namespace kinds {

struct ListText {
	text::Runs runs;
	text::Layout layout;
	float naturalWidth = 0, width = 0;
};
struct TabText {
	std::string id;
	ListText label;
	float x = 0, width = 0, naturalWidth = 0;
	bool active = false;
};
struct ListTabsCache {
	// App pages' rows (omp's /git file tree): compact, as many rows as fit.
	bool dense = false;
	ListText label, detail, value, icon, empty;
	std::vector<ListText> hints;
	std::vector<TabText> tabs;
	float width = -1, height = 0, naturalWidth = 0;
	float left = 0, right = 0, detailX = 0, valueX = 0, hintsX = 0;
	float hintsWidth = 0, hintGap = 0, valueWidth = 0;
};

bool listTabsKind(tsp::Kind kind);
void syncListTabs(const tsp::Node& node, ListTabsCache& cache, text::Engine& engine, const text::Fonts& fonts);
SDL_FPoint measureListTabs(const tsp::Node& node, ListTabsCache& cache, text::Engine& engine, const text::Fonts& fonts,
                           float width);
bool paintListTabs(const tsp::Node& node, ListTabsCache& cache, text::Engine& engine, const text::Fonts& fonts,
                   SDL_Renderer& renderer, const SDL_FRect& rect, view::HitTargets& hits);
void forgetListTabs(ListTabsCache& cache, text::Engine& engine);
bool listItemMatches(const tsp::Node& node, std::string_view filter);

} // namespace kinds
