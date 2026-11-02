#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"

namespace view {
class HitTargets;
}

namespace kinds {

struct MdCell {
	text::Runs runs;
	text::Layout layout;
	float x = 0, y = 0, width = 0, height = 0;
	int align = 0;
};
struct MdBlock {
	enum class Kind { Paragraph, Heading, Code, Quote, Item, Table, Rule, Picture } kind = Kind::Paragraph;
	size_t from = 0;
	int level = 0;
	std::string text, marker, lang;
	std::vector<std::vector<std::string>> rows;
	std::vector<int> align;
	text::Runs runs, markerRuns;
	text::Layout layout, markerLayout;
	std::vector<MdCell> cells;
	float y = 0, height = 0, indent = 0, width = -1;
	// A closed ```svg fence that parses is drawn as a picture instead of its source.
	bool closed = false;
	std::shared_ptr<const gfx::Image> picture;
};
struct MdCache {
	std::string source, tone;
	json marks;
	std::vector<MdBlock> blocks;
	float width = -1, measuredWidth = 0, height = 0, fontSize = 0;
	size_t reparseFrom = 0, reparseBytes = 0;
};

text::Runs inlineMarkdown(std::string_view source, const text::RunStyle& base = {});
void syncMd(const tsp::Node& node, MdCache& cache, const text::Fonts& fonts);
SDL_FPoint measureMd(MdCache& cache, text::Engine& engine, const text::Fonts& fonts, float width);
bool paintMd(MdCache& cache, text::Engine& engine, const text::Fonts& fonts, SDL_Renderer& renderer,
             const SDL_FRect& rect, view::HitTargets& hits);

} // namespace kinds
