#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"
#include "view/hit_targets.h"

namespace kinds {

struct DataCache {
	struct Text {
		text::Runs runs;
		text::Layout layout;
		text::LayoutOptions options;
		SDL_FPoint at{};
		float naturalWidth = 0;
		bool animated = false;
	};
	struct Column {
		std::string id;
		size_t head = 0;
		std::vector<size_t> cells;
		text::Truncate truncate = text::Truncate::End;
		int align = 0;
		double priority = 0;
		float grow = 1, naturalWidth = 0;
		bool shown = true;
	};
	struct Fold {
		std::string key, item;
		size_t text = 0, end = 0;
		std::optional<size_t> icon;
		int depth = 0;
		bool branch = false, expanded = false, changed = false;
	};
	struct Cell {
		size_t row = 0, col = 0;
		SDL_Color color{};
		std::string tip;
	};
	struct AxisLabel {
		size_t at = 0, text = 0;
	};
	struct Sample {
		double value = 0;
		SDL_Color color{};
		std::optional<size_t> label;
		std::string tip;
	};
	struct Mark {
		double value = 0;
		SDL_Color color{};
		std::string tip;
	};
	struct Box {
		SDL_FRect rect;
		float radius;
		SDL_Color color;
	};
	struct Line {
		SDL_FPoint from, to;
		SDL_FRect bounds;
		SDL_Color color;
		float thickness;
	};
	struct Target {
		SDL_FRect rect;
		view::TargetKind kind;
		std::string_view item;
	};

	std::vector<Text> texts;
	std::vector<size_t> visibleTexts;
	std::vector<Column> columns;
	std::vector<size_t> dropOrder;
	std::vector<Fold> folds;
	std::vector<Cell> cells;
	std::vector<AxisLabel> rowLabels, colLabels;
	std::vector<Sample> series;
	std::vector<Mark> marks;
	std::vector<Box> boxes;
	std::vector<Line> lines;
	std::vector<Target> targets;
	std::optional<size_t> summary;
	tsp::Kind kind = tsp::Kind::Kv;
	uint64_t revision = 0;
	size_t heatRows = 0, heatCols = 0;
	float width = -1, measuredWidth = 0, height = 0, naturalWidth = 0, fontSize = 0;
	float keyWidth = 0, cellSize = 0, plotHeight = 0;
	double low = 0, high = 1;
	SDL_Color fill = text::palette::accent;
	bool synced = false, inlineKv = false, heatmap = false, spark = false, toolOutput = false;
};

bool dataKind(tsp::Kind kind);
void syncData(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts);
SDL_FPoint measureData(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts,
                       float width);
bool paintData(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, view::HitTargets& targets);
// Expand targets carry a cache-owned fold key. The caller dirties Yoga after a successful local toggle.
bool foldData(DataCache& cache, std::string_view item);
void forgetData(DataCache& cache);

} // namespace kinds
