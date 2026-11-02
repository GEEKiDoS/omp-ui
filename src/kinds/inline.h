#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"
#include "view/hit_targets.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace kinds {

struct InlineText {
	text::Runs runs;
	text::Layout layout;
	float naturalWidth = 0;
	float width = -1;
};
struct InlinePart {
	float from = 0, to = 0;
	SDL_Color color{};
	bool hatch = false;
	std::string label;
};
struct InlineMark {
	float at = 0;
	SDL_Color color{};
	InlineText glyph;
	std::string title;
};
struct InlineCache {
	InlineText text, label, total;
	std::vector<InlineText> glyphs;
	std::vector<InlinePart> parts;
	std::vector<InlineMark> marks;
	std::array<char, 48> elapsedText{};
	SDL_Color color{};
	float width = -1, naturalWidth = 0, height = 0;
	float trackNaturalWidth = 0, trackWidth = 0, trackOffset = 0;
	float labelOffset = 0, totalOffset = 0;
	float value = 0, thickness = 0;
	float pad = 0, keyWidth = 0;
	double age = 0, stoppedAt = 0;
	Uint64 nextWake = 0, textWake = 0;
	int steps = 10, rung = -1;
	bool known = false, stopped = false, clock = false;
	bool context = false, send = false, hasParts = false, textAnimated = false;
	bool sizeChanged = false;
	enum class MeterStyle { Bar, Ring, Blocks } meterStyle = MeterStyle::Bar;
};

std::string keyCap(std::string_view key);
bool inlineKind(tsp::Kind kind);
void syncInline(const tsp::Node& node, InlineCache& cache, text::Engine& engine, const text::Fonts& fonts);
SDL_FPoint measureInline(const tsp::Node& node, InlineCache& cache, text::Engine& engine, const text::Fonts& fonts,
                         float width);
// nextWake is an absolute SDL_GetTicks() deadline; zero means no visible animation.
// Consume sizeChanged to dirty Yoga when a running elapsed label changes intrinsic width.
bool paintInline(const tsp::Node& node, InlineCache& cache, text::Engine& engine, const text::Fonts& fonts,
                 SDL_Renderer& renderer, const SDL_FRect& rect, view::HitTargets& hits);

} // namespace kinds
