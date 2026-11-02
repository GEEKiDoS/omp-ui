#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"

#include <string>
#include <vector>

namespace view {
class HitTargets;
}

namespace kinds {

struct AnsiState {
	SDL_Color foreground{}, background{};
	bool bold = false, dim = false, italic = false, underline = false, inverse = false, strike = false;
	std::string href;
};

struct AnsiLine {
	text::Runs runs;
	text::Layout layout;
	float layoutWidth = -1, naturalWidth = -1, widest = 0, top = 0;
};

struct AnsiCache {
	std::string source;
	std::vector<AnsiLine> lines;
	AnsiState lastLineState;
	size_t lastLineStart = 0, lastLine = 0, dirtyFrom = 0, naturalDirtyFrom = 0;
	size_t lineCount = 0, hiddenLines = 0;
	text::Runs earlier;
	text::Layout earlierLayout;
	float preferredWidth = 1000000, contentWidth = -1, measuredWidth = 0, measuredHeight = 0;
	float contentHeight = 0, fontSize = 0, lineHeight = 0, ascent = 0, padX = 0, padY = 0;
	int previewLines = 0;
	bool expanded = false, chromeless = false, follow = false;
};

struct RowsCache {
	struct Row {
		std::string source;
		AnsiLine line;
	};
	std::vector<Row> rows;
	size_t dirtyFrom = 0, naturalDirtyFrom = 0;
	float preferredWidth = 1000000, contentWidth = -1, measuredWidth = 0, measuredHeight = 0;
	float fontSize = 0, lineHeight = 0, ascent = 0;
	bool expanded = false;
};

void syncAnsi(const tsp::Node& node, AnsiCache& cache, text::Engine& engine, const text::Fonts& fonts, bool chromeless);
SDL_FPoint measureAnsi(const tsp::Node& node, AnsiCache& cache, text::Engine& engine, const text::Fonts& fonts,
                       float width, bool expanded);
bool paintAnsi(const tsp::Node& node, AnsiCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, bool expanded, view::HitTargets& hits);

void syncRows(const tsp::Node& node, RowsCache& cache, text::Engine& engine, const text::Fonts& fonts, bool chromeless);
SDL_FPoint measureRows(const tsp::Node& node, RowsCache& cache, text::Engine& engine, const text::Fonts& fonts,
                       float width, bool expanded);
bool paintRows(const tsp::Node& node, RowsCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, bool expanded, view::HitTargets& hits);

} // namespace kinds
