#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"

namespace view {
class HitTargets;
}

namespace kinds {

text::Runs highlightLine(std::string_view line, std::string_view language);

struct CodeCache {
	enum class LineKind { Code, Context, Insert, Delete, Hunk, Meta };
	struct DiffState {
		int oldNumber = 1, newNumber = 1;
		int oldRemaining = 0, newRemaining = 0;
		bool inHunk = false;
	};
	struct Line {
		size_t begin = 0, end = 0;
		LineKind kind = LineKind::Code;
		DiffState after;
		text::Runs runs, oldNumber, newNumber, sign;
		text::Layout layout, previewLayout, oldNumberLayout, newNumberLayout, signLayout;
		SDL_Color background{0, 0, 0, 0};
		int oldValue = 0, newValue = 0;
		bool hasOld = false, hasNew = false;
		float layoutWidth = -1, y = 0, visibleHeight = 0;
		size_t previewLines = 0;
	};

	std::string source, language;
	std::vector<Line> lines;
	text::Runs header, more;
	text::Layout headerLayout, moreLayout;
	uint64_t revision = 0;
	size_t tailByte = 0, tailLine = 0, visibleLines = 0, hiddenLines = 0;
	float width = -1, measuredWidth = 0, fontSize = 0, height = 0;
	float padX = 0, padY = 0, gutter = 0, oldColumn = 0, newColumn = 0, signColumn = 0;
	float headerHeight = 0, bodyHeight = 0, contentHeight = 0, moreHeight = 0;
	int start = 1, preview = 0;
	bool synced = false, dirty = true, diff = false, chromeless = false;
	bool numbers = false, wrap = true, expanded = false;
};

// Code and Diff share this cache and dispatch on node.kind; Diff is always unified.
void syncCode(const tsp::Node& node, CodeCache& cache, text::Engine& engine, const text::Fonts& fonts, bool chromeless);
SDL_FPoint measureCode(const tsp::Node& node, CodeCache& cache, text::Engine& engine, const text::Fonts& fonts,
                       float width, bool expanded);
bool paintCode(const tsp::Node& node, CodeCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, bool expanded, view::HitTargets& targets);
void forgetCode(CodeCache& cache, text::Engine& engine);

} // namespace kinds
