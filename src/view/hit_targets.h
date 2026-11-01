#pragma once

#include "core/tsp.h"
#include "gfx/draw.h"
#include <string_view>
#include <vector>

namespace view {

enum class TargetKind {
	Node,
	Toggle,
	Select,
	Activate,
	Action,
	Change,
	Number,
	Focus,
	Link,
	Expand,
	Hover,
	Copy,
	Phase,
	Block
};
struct HitTarget {
	SDL_FRect rect;
	tsp::NodeId node;
	TargetKind kind;
	std::string_view item;
	std::optional<json> value;
};

// Paint order is hit order: parents first, their sub-targets and children after.
class HitTargets {
public:
	void clear() {
		if (!frozen_)
			entries_.clear();
	}
	// A partial redraw repaints only damaged areas; the targets from the last full paint stay
	// valid (nothing moved) and must not be replaced by the damaged areas' subset.
	void freeze(bool frozen) { frozen_ = frozen; }
	void bind(tsp::NodeId node, const SDL_FRect& clip) {
		node_ = node;
		clip_ = clip;
	}
	void add(SDL_FRect rect, TargetKind kind, std::string_view item = {}, tsp::NodeId owner = 0,
	         std::optional<json> value = {}) {
		rect = gfx::intersect(rect, clip_);
		if (!frozen_ && rect.w > 0 && rect.h > 0)
			entries_.push_back({rect, owner ? owner : node_, kind, item, std::move(value)});
	}
	const std::vector<HitTarget>& entries() const { return entries_; }
	std::optional<std::reference_wrapper<const HitTarget>> hit(float x, float y) const {
		for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
			if (x >= it->rect.x && y >= it->rect.y && x < it->rect.x + it->rect.w && y < it->rect.y + it->rect.h)
				return *it;
		return {};
	}

private:
	std::vector<HitTarget> entries_;
	tsp::NodeId node_ = 0;
	SDL_FRect clip_{};
	bool frozen_ = false;
};

} // namespace view
