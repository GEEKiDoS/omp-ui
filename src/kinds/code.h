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
	// One source line. Its line numbers and diff sign are drawn at paint time from the values
	// (only visible lines need them), so a line holds just its text runs and their layout.
	struct Line {
		size_t begin = 0, end = 0;
		LineKind kind = LineKind::Code;
		DiffState after;
		text::Runs runs;
		text::Layout layout;
		SDL_Color background{0, 0, 0, 0};
		int oldValue = 0, newValue = 0;
		bool hasOld = false, hasNew = false;
		float layoutWidth = -1, y = 0, visibleHeight = 0;
	};

	std::string source, language;
	std::vector<Line> lines;
	text::Runs header, more;
	text::Layout headerLayout, moreLayout;
	uint64_t revision = 0;
	size_t tailByte = 0, tailLine = 0, visibleLines = 0, hiddenLines = 0;
	// A clamped preview cuts at most one line short: its first rows, laid out once.
	text::Layout partial;
	size_t partialLine = std::string::npos;
	float width = -1, measuredWidth = 0, fontSize = 0, height = 0;
	float padX = 0, padY = 0, gutter = 0, oldColumn = 0, newColumn = 0, signColumn = 0;
	float headerHeight = 0, bodyHeight = 0, contentHeight = 0, moreHeight = 0;
	int start = 1, preview = 0;
	// Collapsed previews show at most `budget` pixels of the block (0: no limit); lines past it
	// get an estimated height instead of a layout, and only the first `laidLines` are laid out.
	float budget = 0, laidBudget = 0;
	size_t laidLines = 0;
	// `mode: "split"` diffs: old on the left, new on the right. Each row holds a line per side
	// (npos: none on that side; a context line sits on both); hunk/meta rows span both.
	struct SplitRow {
		size_t left = std::string::npos, right = std::string::npos;
		float y = 0, height = 0;
		bool full = false;
	};
	bool split = false;
	std::vector<SplitRow> rows;
	float half = 0, sideGutter = 0;
	bool synced = false, dirty = true, diff = false, chromeless = false;
	bool numbers = false, wrap = true, expanded = false;
};

// Code and Diff share this cache and dispatch on node.kind; Diff is always unified.
void syncCode(const tsp::Node& node, CodeCache& cache, const text::Fonts& fonts, bool chromeless);
SDL_FPoint measureCode(const tsp::Node& node, CodeCache& cache, text::Engine& engine, const text::Fonts& fonts,
                       float width, bool expanded);
bool paintCode(const tsp::Node& node, CodeCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, bool expanded, view::HitTargets& targets);

} // namespace kinds
