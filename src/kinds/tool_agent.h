#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"

#include <functional>

namespace view {
class HitTargets;
}

namespace kinds {

struct ToolAgentCache {
	struct Row {
		text::Runs runs;
		text::Layout layout;
		SDL_FRect rect{};
		float naturalWidth = 0;
		bool effects = false;
	};
	struct Chip {
		Row label;
		SDL_Color color = text::palette::pending;
		std::string tip;
	};
	struct Button {
		Row label;
		std::string id, tip;
		bool disabled = false, primary = false, danger = false, on = false, copy = false;
	};
	struct Item {
		Row mark, label, note;
		bool closed = false;
	};
	struct Phase {
		std::string id;
		Row title;
		std::vector<Item> items;
		bool collapsed = false;
		std::optional<bool> localCollapsed;
	};

	Row mark, head, after, right, clock, intent, currentTool, currentToolAge, retry, retryClock, hud;
	std::vector<Chip> chips;
	std::vector<Button> buttons;
	std::vector<Phase> phases;
	std::string command, completedIntent, mode;
	uint64_t revision = 0;
	int64_t clockSecond = -1, toolSecond = -1, retrySecond = -1;
	double age = 0, took = 0, toolAge = 0, retryAge = 0, retryDelay = 0;
	float width = -1, fontSize = 0, measuredWidth = 0, ownHeight = 0;
	float childLeft = 0, childRight = 0, bottomInset = 0, indent = 0, progress = -1;
	SDL_FRect headRect{}, headContentRect{}, progressRect{};
	SDL_Color statusColor = text::palette::muted;
	bool synced = false, dirty = true, running = false, inlineFrame = false;
	bool collapsible = false, clocked = false, hasTook = false, toolClocked = false, retryClocked = false;
	bool laidOutChromeless = false;

	// Set by the view; retained layout does not depend on pointer movement.
	bool headHovered = false, chromeless = false;
};

void syncToolAgent(const tsp::Node& node, ToolAgentCache& cache, text::Engine& engine, const text::Fonts& fonts);
// Own size excludes children. ownHeight reserves the padded header/body and the
// gap before children; bottomInset and childLeft/childRight reserve their insets.
SDL_FPoint measureToolAgent(const tsp::Node& node, ToolAgentCache& cache, text::Engine& engine,
                            const text::Fonts& fonts, float width);
bool paintToolAgent(const tsp::Node& node, ToolAgentCache& cache, text::Engine& engine, const text::Fonts& fonts,
                    SDL_Renderer& renderer, const SDL_FRect& rect, bool collapsed, view::HitTargets& targets);
void forgetToolAgent(ToolAgentCache& cache, text::Engine& engine);
// Handles Phase hit items (the exact phase id) locally.
// The caller dirties the measured Yoga leaf when this returns true.
bool toggleChecklistPhase(ToolAgentCache& cache, std::string_view item);
// Visits the text rows a card shows at rest (head, status, chips, checklist items), with their
// leaf-local rects, for the transcript overview; hover-only buttons are left out.
void eachShownRow(const ToolAgentCache& cache,
                  const std::function<void(const text::Runs&, const text::Layout&, const SDL_FRect&)>& visit);

} // namespace kinds
