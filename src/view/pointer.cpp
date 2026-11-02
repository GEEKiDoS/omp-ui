#include "pointer.h"
#include "kinds/chrome.h"
#include "core/text_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace view {
namespace {
bool contains(const SDL_FRect& rect, float x, float y) {
	return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}
float clampPosition(float value, float start, float extent, float size) {
	return std::clamp(value, start, start + std::max(0.0f, extent - size));
}
void identify(json& node, size_t& serial) {
	node["id"] = "hover." + std::to_string(++serial);
	if (auto children = node.find("c"); children != node.end() && children->is_array())
		for (auto& child : *children)
			identify(child, serial);
}
} // namespace

void Tree::setPreedit(std::string_view value, int start, int length) {
	if (const auto focus = doc_.focus())
		kinds::setPreedit(at(*focus).leaf.preedit, engine_.get(), value, start, length);
}

bool Tree::hoverTool(float x, float y) {
	tsp::NodeId next = 0;
	if (const auto target = hit(x, y)) {
		for (auto current = target->get().node; current; current = doc_.at(current).parent) {
			const auto& view = at(current);
			if (doc_.at(current).kind != tsp::Kind::Tool || !view.leaf.toolAgent)
				continue;
			auto header = view.leaf.toolAgent->headRect;
			header.x += view.rect.x;
			header.y += view.rect.y;
			if (contains(header, x, y) && contains(view.hitRect, x, y))
				next = current;
			break;
		}
	}
	if (next == hoveredTool_)
		return false;
	if (hoveredTool_)
		at(hoveredTool_).leaf.toolAgent->headHovered = false;
	hoveredTool_ = next;
	if (next)
		at(next).leaf.toolAgent->headHovered = true;
	return true;
}

bool Tree::click(const HitTarget& target, std::string_view gesture, tsp::Session& session) {
	if (target.kind == TargetKind::Block)
		return true;
	const bool primary = gesture == "click";
	const bool doubleClick = gesture == "dblclick";
	auto send = [&](tsp::NodeId id, std::string_view event, std::string_view item = {}) {
		json value{{"ev", event}, {"sf", surface_.id}, {"id", doc_.at(id).id}};
		if (!item.empty())
			value[event == "action" ? "act" : "item"] = item;
		if ((event == "action" || event == "change") && target.value)
			value["value"] = *target.value;
		session.sendEvent(value);
	};
	auto toggle = [&](tsp::NodeId id) {
		toggleNode(id, session);
	};
	if (target.kind == TargetKind::Link && primary) {
		SDL_OpenURL(std::string(target.item).c_str());
		return true;
	}
	if (target.kind == TargetKind::Copy && primary) {
		SDL_SetClipboardText(std::string(target.item).c_str());
		return true;
	}
	if (target.kind == TargetKind::Phase && primary) {
		auto& view = at(target.node);
		if (kinds::toggleChecklistPhase(*view.leaf.toolAgent, target.item))
			YGNodeMarkDirty(view.yoga.get());
		return true;
	}
	if (target.kind == TargetKind::Expand && primary) {
		auto& view = at(target.node);
		if (view.leaf.data) {
			if (kinds::foldData(*view.leaf.data, target.item))
				YGNodeMarkDirty(view.yoga.get());
			return true;
		}
		// Inside a folded card, "N more lines" opens the card (its arrow turns), which shows
		// the block in full; elsewhere the block expands on its own.
		if (const auto card = foldingAncestor(target.node); card && collapsed(*card)) {
			toggleNode(*card, session);
			return true;
		}
		view.leaf.expanded = !view.leaf.expanded;
		view.leaf.width = -1;
		if (YGNodeHasMeasureFunc(view.yoga.get()))
			YGNodeMarkDirty(view.yoga.get());
		return true;
	}
	if (target.kind == TargetKind::Toggle && primary) {
		toggle(target.node);
		return true;
	}
	if (target.kind == TargetKind::Select && (primary || doubleClick)) {
		if (target.value && target.value->is_object() && target.value->contains("item") && primary) {
			session.sendEvent({{"ev", "change"},
			                   {"sf", surface_.id},
			                   {"id", doc_.at(target.node).id},
			                   {"item", target.value->at("item")},
			                   {"value", target.value->at("value")}});
			return true;
		}
		send(target.node, doubleClick ? "activate" : "select", target.item);
		return true;
	}
	if (target.kind == TargetKind::Activate && primary) {
		if (const auto& node = doc_.at(target.node); node.kind == tsp::Kind::Prefs) {
			// Choice pills act as comboboxes: a pill closes its own open menu, and switches from
			// another row's (omp ignores `activate` while another row is open, so that row is
			// closed first by selecting this one).
			const auto& editing = node.prop("editing");
			const auto open = editing.is_object() ? editing.value("row", "") : std::string{};
			if (open == target.item) {
				session.sendEvent(
				    {{"ev", "action"}, {"sf", surface_.id}, {"id", node.id}, {"act", "close"}, {"value", open}});
				return true;
			}
			if (!open.empty())
				send(target.node, "select", target.item);
		}
		send(target.node, "activate", target.item);
		return true;
	}
	if (target.kind == TargetKind::Focus && primary) {
		send(target.node, "focus");
		return true;
	}
	if (target.kind == TargetKind::Change && primary) {
		send(target.node, "change", target.item);
		return true;
	}
	if (target.kind == TargetKind::Action && primary) {
		send(target.node, "action", target.item);
		return true;
	}
	for (auto current = target.node; current; current = doc_.at(current).parent) {
		const auto& node = doc_.at(current);
		if (primary && (node.kind == tsp::Kind::Editor || node.kind == tsp::Kind::Input) && doc_.focus() != current) {
			send(current, "focus");
			return true;
		}
		const auto& actions = node.prop("actions");
		const auto action = actions.is_object() ? actions.find(std::string(gesture)) : actions.end();
		if (action == actions.end()) {
			if (primary && node.flag("collapsible") && at(current).headerNode
			    && descendant(target.node, at(current).headerNode)) {
				toggle(current);
				return true;
			}
			continue;
		}
		if (gesture == "menu")
			return false; // Menu entries are offered by the context menu, one at a time.
		if (action->is_array()) {
			for (const auto& item : *action)
				if (item.is_string())
					runAction(current, item.get<std::string>(), session);
		} else if (action->is_string())
			runAction(current, action->get<std::string>(), session);
		return true;
	}
	return false;
}

void Tree::toggleNode(tsp::NodeId id, tsp::Session& session) {
	at(id).collapsed = !collapsed(id);
	// Blocks inside show in full exactly while the card is open (see blockExpanded()); this also
	// holds for blocks omp sends again in response to the toggle.
	remeasureLeaves(id);
	sync(id);
	display(id);
	const auto& node = doc_.at(id);
	json event{{"ev", "toggle"}, {"sf", surface_.id}, {"id", node.id}, {"collapsed", collapsed(id)}};
	if (!node.str("key").empty())
		event["key"] = node.str("key");
	session.sendEvent(event);
}

void Tree::runAction(tsp::NodeId id, std::string_view name, tsp::Session& session) {
	const auto& node = doc_.at(id);
	if (name == "toggle")
		toggleNode(id, session);
	else if (name == "open" && !node.str("href").empty())
		SDL_OpenURL(std::string(node.str("href")).c_str());
	else if (name == "copy") {
		const auto value =
		    node.prop("text").is_null() ? text::plainText(node.prop("spans")) : text::plainText(node.prop("text"));
		SDL_SetClipboardText(value.c_str());
	} else if ((name == "select" || name == "activate") && node.parent && doc_.at(node.parent).kind == tsp::Kind::List)
		session.sendEvent({{"ev", name}, {"sf", surface_.id}, {"id", doc_.at(node.parent).id}, {"item", node.id}});
	else
		session.sendEvent({{"ev", "action"}, {"sf", surface_.id}, {"id", node.id}, {"act", name}});
}

std::vector<std::pair<tsp::NodeId, std::string>> Tree::menuActions(float x, float y) const {
	// The innermost node under the point that declares `actions.menu` supplies the entries.
	const auto target = hit(x, y);
	if (!target)
		return {};
	for (auto current = target->get().node; current; current = doc_.at(current).parent) {
		const auto& actions = doc_.at(current).prop("actions");
		const auto menu = actions.is_object() ? actions.find("menu") : actions.end();
		if (menu == actions.end())
			continue;
		std::vector<std::pair<tsp::NodeId, std::string>> entries;
		if (menu->is_string())
			entries.emplace_back(current, menu->get<std::string>());
		else if (menu->is_array())
			for (const auto& item : *menu)
				if (item.is_string())
					entries.emplace_back(current, item.get<std::string>());
		return entries;
	}
	return {};
}

std::optional<tsp::NodeId> Tree::editorAt(float x, float y) const {
	const auto target = hit(x, y);
	if (!target)
		return std::nullopt;
	for (auto current = target->get().node; current; current = doc_.at(current).parent) {
		const auto kind = doc_.at(current).kind;
		if (kind == tsp::Kind::Editor || kind == tsp::Kind::Input)
			return current;
	}
	return std::nullopt;
}

size_t Tree::editorOffset(tsp::NodeId id, float x, float y) {
	const auto& leaf = at(id).leaf;
	const auto hit = engine_.get().hit(leaf.runs, leaf.layout, x - leaf.textOrigin.x, y - leaf.textOrigin.y);
	return kinds::editorSourceOffset(doc_.at(id), leaf.editorSpans, hit.byte);
}

void Tree::setEditorSelection(tsp::NodeId id, std::optional<std::pair<size_t, size_t>> selection) {
	auto& view = at(id);
	if (view.leaf.editorSelection == selection)
		return;
	view.leaf.editorSelection = selection;
	textChanged(id); // Rebuild the runs with the new highlight.
}

void Tree::sendEdit(tsp::NodeId id, size_t from, size_t to, std::string_view inserted, tsp::Session& session) const {
	// `len` is the text length we saw; the program drops the edit if its text has moved on.
	const auto& node = doc_.at(id);
	session.sendEvent({{"ev", "edit"},
	                   {"sf", surface_.id},
	                   {"id", node.id},
	                   {"from", from},
	                   {"to", to},
	                   {"text", std::string(inserted)},
	                   {"cursor", from + text::utf16Length(inserted)},
	                   {"len", text::utf16Length(node.str("text"))}});
}

bool Tree::scrollAt(float x, float y, float delta) {
	const auto scrollBy = [&](NodeView& view) {
		view.wheelScroll(delta, SDL_GetTicks());
		transitioning |= view.scroll != view.scrollTween.target();
	};
	const auto target = hit(x, y);
	if (!target) {
		// No target under the pointer (plain text in a pane): the innermost scrolling box.
		for (auto it = scrollers_.rbegin(); it != scrollers_.rend(); ++it)
			if (contains(it->first, x, y) && at(it->second).scrollMax > 0) {
				scrollBy(at(it->second));
				return true;
			}
		return false;
	}
	for (auto current = target->get().node; current; current = doc_.at(current).parent) {
		auto& view = at(current);
		if (view.leaf.picker && kinds::scrollPicker(*view.leaf.picker, x, y, delta))
			return true;
		if (view.leaf.prefs && kinds::scrollPrefs(*view.leaf.prefs, x, y, delta))
			return true;
		if (view.scrollMax <= 0 || (doc_.at(current).id == "main" && doc_.at(current).parent == doc_.root()))
			continue;
		scrollBy(view);
		return true;
	}
	for (const auto& entry : targets_.entries())
		if (entry.kind == TargetKind::Block && contains(entry.rect, x, y))
			return true;
	return false;
}

bool Tree::paintLayer(const SDL_FRect& viewport, float dockY, float asideLayoutWidth, SDL_Renderer& renderer,
                      gfx::ClipStack& clips) {
	const auto layer = doc_.find("layer");
	if (!layer)
		return false;
	const float em = fonts_.size(), scale = em / 15;
	const auto& children = doc_.at(*layer).children;
	const bool modal = std::any_of(children.begin(), children.end(), [&](auto id) {
		const auto& node = doc_.at(id);
		return !node.flag("hidden") && node.kind != tsp::Kind::Toast && node.kind != tsp::Kind::Prefs
		    && (node.kind != tsp::Kind::Overlay || node.flag("modal"));
	});
	if (modal) {
		gfx::fill(renderer, viewport, text::rgb(0, 110));
		targets_.bind(*layer, viewport);
		targets_.add(viewport, TargetKind::Block);
	}
	bool animating = false;
	for (const auto id : children) {
		const auto& node = doc_.at(id);
		if (node.flag("hidden") || node.kind == tsp::Kind::Toast)
			continue;
		auto& view = at(id);
		const auto yoga = view.yoga.get();
		const bool prefs = node.kind == tsp::Kind::Prefs, picker = node.kind == tsp::Kind::Picker;
		const auto size = node.str("size", "md");
		const bool full = size == "full" || size == "screen";
		const float fraction = size == "sm" ? 0.42f : size == "lg" ? 0.82f : 0.62f;
		const float maxWidth = std::max(1.0f, viewport.w - em * gfx::space::lg * 2);
		float width = prefs ? asideLayoutWidth
		            : full  ? viewport.w
		                    : std::min(maxWidth, std::max(em * 26, viewport.w * fraction));
		const auto& anchor = node.prop("anchor");
		const bool overlay = node.kind == tsp::Kind::Overlay;
		if (overlay && anchor.is_object() && anchor.contains("caret"))
			width = std::min(width, em * 34);
		const float maxHeight =
		    prefs || (picker && size == "screen") ? viewport.h : std::max(1.0f, viewport.h - em * gfx::space::lg * 2);
		YGNodeStyleSetWidth(yoga, width);
		if (picker) {
			kinds::layoutPicker(node, *view.leaf.picker, engine_.get(), fonts_, width, maxHeight);
			const float naturalHeight = std::max(maxHeight, view.leaf.picker->minimumHeight);
			YGNodeStyleSetHeight(yoga, naturalHeight);
			kinds::layoutPicker(node, *view.leaf.picker, engine_.get(), fonts_, width, naturalHeight);
		} else if (prefs)
			YGNodeStyleSetHeight(yoga, maxHeight);
		if (laidOut || view.paintDirty || YGNodeIsDirty(yoga) || YGNodeLayoutGetWidth(yoga) != width)
			YGNodeCalculateLayout(yoga, width, YGUndefined, YGDirectionLTR);
		const float naturalHeight = YGNodeLayoutGetHeight(yoga);
		const float height = std::min(maxHeight, naturalHeight);
		float x = prefs ? viewport.x + viewport.w - width : viewport.x + (viewport.w - width) * 0.5f;
		float y = prefs ? viewport.y : viewport.y + (viewport.h - height) * 0.5f;
		if (overlay && anchor == "top")
			y = viewport.y + em * gfx::space::lg;
		else if (overlay && anchor == "bottom")
			y = dockY - height - em * gfx::space::sm;
		else if (overlay && anchor.is_object()) {
			std::optional<SDL_FRect> target;
			if (const auto caret = anchor.find("caret"); caret != anchor.end() && caret->is_string()) {
				if (const auto editor = doc_.find(caret->get_ref<const std::string&>()))
					target = at(*editor).caret;
				if (target) {
					x = target->x;
					y = target->y - height - em * gfx::space::sm;
				}
			} else if (const auto name = anchor.find("node"); name != anchor.end() && name->is_string()) {
				if (const auto anchored = doc_.find(name->get_ref<const std::string&>()))
					target = at(*anchored).rect;
				if (target) {
					x = target->x;
					y = anchor.value("side", "below") == "above" ? target->y - height : target->y + target->h;
				}
			}
		}
		x = clampPosition(x, viewport.x, viewport.w, width);
		y = clampPosition(y, viewport.y, viewport.h, height);
		const SDL_FRect finalBox{x, y, width, height};
		const float progress = reveal(view, prefs ? 180 : 140);
		if (prefs && progress == 0) {
			// prepareAside reserved the first frame before a possibly expensive re-wrap.
			// Start its clock now, so that layout cannot consume the slide itself.
			view.reveal.snap(0);
			view.reveal.to(1, SDL_GetTicks(), 180);
		}
		if (prefs)
			x += width * (1 - progress);
		else
			y += em * 0.6f * (1 - progress);
		const SDL_FRect box{x, y, width, height};
		view.scrollMax = std::max(0.0f, naturalHeight - height);
		view.sampleScroll(SDL_GetTicks());
		transitioning |= view.scroll != view.scrollTween.target();
		if (progress != 1)
			clips.push(finalBox);
		if (!prefs) {
			gfx::rounded(renderer, {box.x, box.y + em * 0.25f, box.w, box.h}, em * 0.7f, text::rgb(0, 60));
			gfx::rounded(renderer, box, em * 0.65f, text::palette::surface);
		}
		clips.push(box);
		const float leafScroll = !picker && !prefs && YGNodeHasMeasureFunc(yoga) ? view.scroll : 0;
		animating |=
		    paintNode(id, x - YGNodeLayoutGetLeft(yoga), y - YGNodeLayoutGetTop(yoga) - leafScroll, renderer, clips);
		clips.pop();
		if (!prefs)
			gfx::roundedBorder(renderer, box, em * 0.65f, text::palette::border, scale);
		if (view.scrollMax > 0) {
			const float thumb = std::max(em, height * height / naturalHeight);
			gfx::rounded(renderer,
			             {x + width - 4 * scale, y + (height - thumb) * view.scroll / view.scrollMax, 2 * scale, thumb},
			             scale, text::palette::dim);
		}
		if (progress != 1)
			clips.pop();
	}
	float toastY = dockY - em;
	for (const auto id : children) {
		const auto& node = doc_.at(id);
		if (node.kind != tsp::Kind::Toast || node.flag("hidden"))
			continue;
		auto& view = at(id);
		view.leaf.opacity = kinds::toastOpacity(node, view.arrived);
		if (view.leaf.opacity <= 0)
			continue;
		const auto yoga = view.yoga.get();
		const float width = std::min(viewport.w - 2 * em, YGNodeLayoutGetWidth(yoga)),
		            height = YGNodeLayoutGetHeight(yoga);
		toastY -= height;
		animating |= paint(node.id, viewport.x + viewport.w - em - width, toastY, renderer, clips);
		toastY -= em * 0.4f;
		const double age =
		    node.stamps.contains("text") ? node.since("text") : static_cast<double>(SDL_GetTicks() - view.arrived);
		const double fadeIn = node.num("ttl", 4000) - age - 400;
		if (fadeIn > 0)
			wakeAt(SDL_GetTicks() + static_cast<Uint64>(std::ceil(fadeIn)));
		else
			animating = true;
	}
	return animating;
}

void Views::dismissHover() {
	auto& state = *pointer_;
	if (state.shown)
		state.lastCard = SDL_GetTicks();
	state.shown = false;
	state.node.clear();
	state.surface.clear();
	state.card.reset();
	state.cardSurface.doc.reset();
	state.title.clear();
	state.cardRect = {};
	state.tip.clear();
	state.tipRect.reset();
	state.reveal.snap(1);
}

bool Views::motion(float x, float y, bool refresh) {
	auto& state = *pointer_;
	state.position = {x, y};
	if (state.number && !refresh) {
		const auto found = trees_.find(state.number->surface);
		if (found == trees_.end()) {
			state.number.reset();
			return false;
		}
		auto& tree = *found->second;
		const auto node = tree.document().find(state.number->node);
		if (!node) {
			state.number.reset();
			return false;
		}
		kinds::dragPrefsNumber(*tree.at(*node).leaf.prefs, state.number->item, state.number->control,
		                       state.number->track, state.number->startX, x, fonts_.size() / 15);
		return true;
	}
	bool hoverChanged = false;
	for (auto& tree : painted_)
		hoverChanged |= tree.get().hoverTool(x, y);
	if (refresh && state.suppressed)
		return hoverChanged;
	if (!refresh)
		state.suppressed = false;
	if (state.shown && !state.passThrough && contains(state.cardRect, x, y))
		return hoverChanged;
	std::string_view surface, name, tip;
	std::optional<SDL_FRect> tipRect;
	bool hand = false;
	// The aside's resizable edge takes the pointer before anything under it.
	const bool resize = contains(asideGrip_, x, y);
	for (auto it = painted_.rbegin(); !resize && it != painted_.rend(); ++it) {
		auto& tree = it->get();
		const auto hit = tree.hit(x, y);
		if (!hit)
			continue;
		const auto& target = hit->get();
		hand = target.kind == TargetKind::Link || target.kind == TargetKind::Action || target.kind == TargetKind::Toggle
		    || target.kind == TargetKind::Expand || target.kind == TargetKind::Select
		    || target.kind == TargetKind::Activate || target.kind == TargetKind::Change
		    || target.kind == TargetKind::Number || target.kind == TargetKind::Copy || target.kind == TargetKind::Phase;
		if (!refresh && target.kind == TargetKind::Select && target.value && target.value->is_object()
		    && target.value->contains("item")) {
			if (state.previewItem != target.item) {
				state.previewItem = target.item;
				session_.sendEvent({{"ev", "select"},
				                    {"sf", tree.surface().id},
				                    {"id", tree.document().at(target.node).id},
				                    {"item", target.item}});
				hoverChanged = true;
			}
		} else if (!refresh)
			state.previewItem.clear();
		if (target.kind == TargetKind::Block)
			break;
		if (target.kind == TargetKind::Hover) {
			surface = tree.surface().id;
			name = tree.document().at(target.node).id;
			tip = target.item;
			tipRect = target.rect;
		}
		for (auto current = target.node; current; current = tree.document().at(current).parent) {
			const auto& node = tree.document().at(current);
			hand |= node.prop("actions").is_object() || !node.str("href").empty();
			if (name.empty()
			    && (node.prop("hovercard").is_object()
			        || (node.kind != tsp::Kind::Picker && node.kind != tsp::Kind::Prefs
			            && !node.str("title").empty()))) {
				surface = tree.surface().id;
				name = node.id;
			}
		}
		break;
	}
	const auto cursor = resize ? PointerState::Cursor::Resize
	                  : hand   ? PointerState::Cursor::Hand
	                           : PointerState::Cursor::Arrow;
	if (cursor != state.cursor) {
		// The edge highlights while the pointer is over it.
		hoverChanged |= (cursor == PointerState::Cursor::Resize) != (state.cursor == PointerState::Cursor::Resize);
		state.cursor = cursor;
		if (systemPointer_)
			SDL_SetCursor(cursor == PointerState::Cursor::Resize ? state.resize.get()
			              : cursor == PointerState::Cursor::Hand ? state.pointer.get()
			                                                     : state.arrow.get());
	}
	if (state.surface == surface && state.node == name && state.tip == tip)
		return hoverChanged;
	const std::string nextSurface(surface), nextName(name), nextTip(tip);
	dismissHover();
	state.surface = nextSurface;
	state.node = nextName;
	state.tip = nextTip;
	state.tipRect = tipRect;
	state.since = SDL_GetTicks();
	return true;
}

int Views::hoverWakeDelay() const {
	const auto& state = *pointer_;
	if (state.node.empty() || state.shown)
		return -1;
	const Uint64 delay = state.lastCard && SDL_GetTicks() - state.lastCard < 1000 ? 200 : 350;
	const Uint64 elapsed = SDL_GetTicks() - state.since;
	return elapsed >= delay ? 0 : static_cast<int>(delay - elapsed);
}
int Views::wakeDelay() const {
	const Uint64 now = SDL_GetTicks();
	const auto until = [&](Uint64 tick) {
		return !tick       ? -1
		     : tick <= now ? 0
		                   : static_cast<int>(std::min<Uint64>(tick - now, std::numeric_limits<int>::max()));
	};
	int delay = hoverWakeDelay();
	for (const int other : {until(nextWake_), until(relayoutAt_)})
		if (other >= 0)
			delay = delay < 0 ? other : std::min(delay, other);
	return delay;
}
Views::Wake Views::tick() const {
	if (hoverWakeDelay() == 0 || (relayoutAt_ && relayoutAt_ <= SDL_GetTicks()))
		return Wake::Full;
	return nextWake_ && nextWake_ <= SDL_GetTicks() ? Wake::Animation : Wake::None;
}

bool Views::paintHover(const SDL_FRect& viewport) {
	auto& state = *pointer_;
	if (state.node.empty())
		return false;
	const auto surface = trees_.find(state.surface);
	if (surface == trees_.end()) {
		dismissHover();
		return false;
	}
	auto& tree = *surface->second;
	const auto id = tree.document().find(state.node);
	if (!id) {
		dismissHover();
		return false;
	}
	const auto& node = tree.document().at(*id);
	const auto anchor = state.tipRect.value_or(tree.at(*id).hitRect);
	if (anchor.w <= 0 || anchor.h <= 0) {
		dismissHover();
		return false;
	}
	const float em = fonts_.size(), padX = em * gfx::space::md, gap = em * gfx::space::sm;
	if (!state.shown) {
		if (hoverWakeDelay() != 0)
			return false;
		state.shown = true;
		state.reveal.snap(0);
		state.reveal.to(1, SDL_GetTicks(), 140);
		// The HUD's todo pill opens the full checklist; other nodes bring their own `hovercard`.
		json wire = node.prop("hovercard");
		if (state.tip.empty() && node.kind == tsp::Kind::Checklist && node.str("mode") == "hud") {
			wire = {{"k", "checklist"}, {"p", node.p}};
			wire["p"].erase("mode");
			wire["p"].erase("role");
		}
		state.passThrough = wire.is_object() && wire.value("k", "") == "md";
		if (wire.is_object() && state.tip.empty()) {
			size_t serial = 0;
			identify(wire, serial);
			state.cardSurface.id = "hover";
			state.cardSurface.doc = std::make_unique<tsp::Document>("hover");
			const auto errors = state.cardSurface.doc->applyFrame(
			    1, json::array({json::array({"add", wire["id"], "hover", nullptr, wire})}));
			if (!errors.empty()) {
				dismissHover();
				return false;
			}
			state.card = std::make_unique<Tree>(state.cardSurface, engine_.get(), fonts_);
		} else
			state.title = state.tip.empty() ? text::toRuns(node.prop("title")) : text::toRuns(state.tip);
	}
	const float padY = em * (state.card ? gfx::space::md : gfx::space::sm);
	const float maxWidth = std::max(1.0f, viewport.w - em * gfx::space::lg * 2);
	float width = std::min(maxWidth, em * 32), height = 0;
	if (state.card) {
		state.card->layout(std::max(1.0f, width - padX * 2));
		height = state.card->height("hover.1") + padY * 2;
	} else {
		state.titleLayout = engine_.get().layout(state.title, {.width = std::max(1.0f, width - padX * 2)});
		width = std::min(width, state.titleLayout.width + padX * 2);
		height = state.titleLayout.height + padY * 2;
	}
	height = std::min(height, std::max(1.0f, viewport.h - em * gfx::space::lg * 2));
	const float x = clampPosition(anchor.x, viewport.x, viewport.w, width);
	float y = anchor.y + anchor.h + gap;
	if (y + height > viewport.y + viewport.h)
		y = anchor.y - height - gap;
	y = clampPosition(y, viewport.y, viewport.h, height);
	const SDL_FRect finalBox{x, y, width, height};
	const float progress = state.reveal.value(SDL_GetTicks());
	const bool moving = progress != 1;
	transitioning_ |= moving;
	y += em * 0.4f * (1 - progress);
	state.cardRect = gfx::intersect({x, y, width, height}, finalBox);
	if (moving)
		clips_->push(finalBox);
	gfx::rounded(renderer_.get(), {x, y, width, height}, em * 0.5f, text::palette::surface2);
	gfx::roundedBorder(renderer_.get(), {x, y, width, height}, em * 0.5f, text::palette::border, em / 15);
	const SDL_FRect content{x + padX, y + padY, std::max(0.0f, width - padX * 2), std::max(0.0f, height - padY * 2)};
	clips_->push(content);
	bool animating = false;
	if (state.card) {
		state.card->beginPaint();
		animating = state.card->paint("hover.1", content.x, content.y, renderer_.get(), *clips_);
	} else
		engine_.get().draw(state.title, state.titleLayout, content.x, content.y);
	clips_->pop();
	if (moving)
		clips_->pop();
	return animating;
}

bool Views::click(float x, float y, std::string_view gesture) {
	for (auto it = painted_.rbegin(); it != painted_.rend(); ++it) {
		auto& tree = it->get();
		if (const auto target = tree.hit(x, y)) {
			tree.click(target->get(), gesture, session_);
			return true;
		}
	}
	return false;
}

bool Views::handle(const SDL_Event& event, float toOutputX, float toOutputY) {
	const bool hadHover = pointer_->shown || !pointer_->node.empty();
	switch (event.type) {
	case SDL_EVENT_MOUSE_MOTION:
		if (asideDrag_) {
			// The edge follows the pointer; the width is kept within what the window allows.
			const float scale = fonts_.size() / 15;
			const float x = event.motion.x * toOutputX;
			const float wanted = asideDrag_->second + (asideDrag_->first - x) / scale;
			const float most = std::max(300.0f, lastViewport_.w / scale - 360);
			asidePreferred_ = std::clamp(wanted, 300.0f, most);
			return true;
		}
		if (minimapDrag_) {
			// The slider follows the pointer: scroll moves by the pointer's travel over the
			// slider's own range (or 1/scale when the whole transcript fits the strip).
			const float y = event.motion.y * toOutputY, slider = mainClip_.h * minimapScale_;
			const float max = std::max(0.0f, contentHeight_ - mainClip_.h);
			const float travel = contentHeight_ * minimapScale_ > minimap_.h && max > 0
			                       ? std::max(1.0f, minimap_.h - slider) / max
			                       : minimapScale_;
			scrollMainTo(minimapDrag_->second + (y - minimapDrag_->first) / std::max(travel, 1e-6f));
			return true;
		}
		if (editorDrag_) {
			if (const auto editor = resolve(editorDrag_->ref)) {
				auto& [tree, id] = *editor;
				const size_t offset =
				    tree.get().editorOffset(id, event.motion.x * toOutputX, event.motion.y * toOutputY);
				if (offset != editorDrag_->anchor || editorDrag_->moved) {
					editorDrag_->moved = true;
					selectEditor(editorDrag_->ref,
					             offset == editorDrag_->anchor
					                 ? std::nullopt
					                 : std::optional(std::pair{std::min(offset, editorDrag_->anchor),
					                                           std::max(offset, editorDrag_->anchor)}));
				}
			}
			return true;
		}
		if (selecting_) {
			// Dragging past the transcript's top or bottom edge scrolls it by the overshoot.
			const float x = event.motion.x * toOutputX, y = event.motion.y * toOutputY;
			const float top = mainClip_.y, bottom = mainClip_.y + mainClip_.h;
			const float over = y < top ? y - top : y > bottom ? y - bottom : 0;
			if (over != 0 && scrollOwner_)
				if (const auto main = scrollOwner_->get().document().find("main")) {
					auto& view = scrollOwner_->get().at(*main);
					view.snapScroll(std::clamp(view.scroll + over, 0.0f, std::max(0.0f, contentHeight_ - mainClip_.h)));
					view.stick = false;
					mainScroll_ = view.scroll;
				}
			selection_->second = {x, std::clamp(y, top, bottom) + mainScroll_};
			return true;
		}
		return motion(event.motion.x * toOutputX, event.motion.y * toOutputY);
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		if (!menu_.empty()) {
			// While the menu is open a press either picks an entry (on release) or dismisses it.
			menuPress_ = menuEntryAt(event.button.x * toOutputX, event.button.y * toOutputY);
			if (!menuPress_)
				menu_.clear();
			return true;
		}
		if (event.button.button == SDL_BUTTON_LEFT
		    && contains(asideGrip_, event.button.x * toOutputX, event.button.y * toOutputY)) {
			const float scale = fonts_.size() / 15;
			// Start from the width shown, which the window may have clamped.
			const float most = std::max(300.0f, lastViewport_.w / scale - 360);
			asidePreferred_ = std::clamp((lastViewport_.w - mainClip_.w) / scale, 300.0f, most);
			asideDrag_ = std::pair{event.button.x * toOutputX, asidePreferred_};
			dismissHover();
			if (systemPointer_)
				SDL_CaptureMouse(true);
			return true;
		}
		if (event.button.button == SDL_BUTTON_LEFT && minimapScale_ > 0
		    && contains(minimap_, event.button.x * toOutputX, event.button.y * toOutputY)) {
			// A press beside the slider brings that part of the transcript to the middle of the
			// view; either way the slider then drags.
			const float y = event.button.y * toOutputY;
			const float sliderTop = minimap_.y + mainScroll_ * minimapScale_ - minimapScroll_;
			if (y < sliderTop || y > sliderTop + mainClip_.h * minimapScale_)
				scrollMainTo((y - minimap_.y + minimapScroll_) / minimapScale_ - mainClip_.h * 0.5f);
			else
				scrollMainTo(mainScroll_);
			minimapDrag_ = std::pair{y, mainScroll_};
			dismissHover();
			if (systemPointer_)
				SDL_CaptureMouse(true);
			return true;
		}
		pointer_->cardPress = pointer_->shown && !pointer_->passThrough
		                   && contains(pointer_->cardRect, event.button.x * toOutputX, event.button.y * toOutputY);
		dismissHover();
		pointer_->suppressed = true;
		const bool hadSelection = hasSelection();
		// A right press keeps the selection: the context menu offers to copy it.
		if (event.button.button == SDL_BUTTON_LEFT) {
			selection_.reset();
			if (editorSelection_)
				selectEditor(*editorSelection_, std::nullopt);
		}
		if (event.button.button == SDL_BUTTON_LEFT && session_.programFeature("edit")) {
			// A press in the focused editor places its caret; dragged it selects, double-clicked a word.
			const float x = event.button.x * toOutputX, y = event.button.y * toOutputY;
			for (auto it = painted_.rbegin(); it != painted_.rend(); ++it) {
				auto& tree = it->get();
				if (!tree.hit(x, y))
					continue;
				const auto editor = tree.editorAt(x, y);
				if (!editor || tree.document().focus() != *editor || tree.document().at(*editor).flag("readonly"))
					break;
				const EditorRef ref{tree.surface().id, tree.document().at(*editor).id};
				const size_t offset = tree.editorOffset(*editor, x, y);
				if (event.button.clicks >= 2) {
					const auto value = tree.document().at(*editor).str("text");
					auto begin = text::byteOffset(value, offset), end = begin;
					auto wordByte = [&](size_t at) {
						const auto b = static_cast<unsigned char>(value[at]);
						return b >= 0x80 || std::isalnum(b) || b == '_';
					};
					while (begin > 0 && wordByte(begin - 1))
						--begin;
					while (end < value.size() && wordByte(end))
						++end;
					if (begin < end)
						selectEditor(ref, std::pair{text::utf16Length(value.substr(0, begin)),
						                            text::utf16Length(value.substr(0, end))});
					return true;
				}
				editorDrag_ = EditorDrag{ref, offset};
				if (systemPointer_)
					SDL_CaptureMouse(true);
				return true;
			}
		}
		if (event.button.button == SDL_BUTTON_LEFT) {
			const float x = event.button.x * toOutputX, y = event.button.y * toOutputY;
			bool targeted = pointer_->cardPress;
			for (auto it = painted_.rbegin(); it != painted_.rend() && !targeted; ++it) {
				auto& tree = it->get();
				const auto target = tree.hit(x, y);
				if (!target)
					continue;
				targeted = tree.interactiveAt(x, y);
				if (target->get().kind == TargetKind::Number) {
					const auto& number = target->get();
					pointer_->number = PointerState::NumberDrag{tree.surface().id,
					                                            tree.document().at(number.node).id,
					                                            std::string(number.item),
					                                            *number.value,
					                                            number.rect,
					                                            x};
					kinds::dragPrefsNumber(*tree.at(number.node).leaf.prefs, number.item, *number.value, number.rect, x,
					                       x, fonts_.size() / 15);
					if (systemPointer_)
						SDL_CaptureMouse(true);
					return true;
				}
			}
			// A press on plain transcript text starts a selection drag; a double click selects the word.
			if (!targeted && contains(mainClip_, x, y)) {
				selection_.emplace(SDL_FPoint{x, y + mainScroll_}, SDL_FPoint{x, y + mainScroll_});
				scrollMainTo(mainScroll_);
				selectWord_ = event.button.clicks >= 2;
				selecting_ = !selectWord_;
				if (selecting_ && systemPointer_)
					SDL_CaptureMouse(true);
				return true;
			}
		}
		return hadHover || hadSelection;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (asideDrag_ && event.button.button == SDL_BUTTON_LEFT) {
			asideDrag_.reset();
			if (systemPointer_)
				SDL_CaptureMouse(false);
			return true;
		}
		if (minimapDrag_ && event.button.button == SDL_BUTTON_LEFT) {
			minimapDrag_.reset();
			if (systemPointer_)
				SDL_CaptureMouse(false);
			return true;
		}
		if (menuPress_) {
			const auto at = menuEntryAt(event.button.x * toOutputX, event.button.y * toOutputY);
			const auto pressed = std::exchange(menuPress_, std::nullopt);
			if (at == pressed && menu_[*at].enabled) {
				const auto entry = menu_[*at];
				menu_.clear();
				runMenuEntry(entry);
			}
			return true;
		}
		if (editorDrag_ && event.button.button == SDL_BUTTON_LEFT) {
			const auto drag = *std::exchange(editorDrag_, std::nullopt);
			if (systemPointer_)
				SDL_CaptureMouse(false);
			// A press without a drag moves the program's caret there; an empty edit changes nothing else.
			if (!drag.moved)
				if (const auto editor = resolve(drag.ref))
					editor->first.get().sendEdit(editor->second, drag.anchor, drag.anchor, {}, session_);
			return true;
		}
		if (event.button.clicks >= 2 && editorSelection_)
			return true;
		if (selectWord_ && selection_ && event.button.clicks >= 2)
			return true;
		if (selecting_ && event.button.button == SDL_BUTTON_LEFT) {
			selecting_ = false;
			if (systemPointer_)
				SDL_CaptureMouse(false);
			// A press without a drag is a plain click: it only cleared the old selection.
			const auto [anchor, focus] = *selection_;
			if (std::abs(anchor.x - focus.x) + std::abs(anchor.y - focus.y) < fonts_.size() * 0.3f)
				selection_.reset();
			return true;
		}
		if (pointer_->number && event.button.button == SDL_BUTTON_LEFT) {
			auto drag = std::move(*pointer_->number);
			pointer_->number.reset();
			if (systemPointer_)
				SDL_CaptureMouse(false);
			const auto found = trees_.find(drag.surface);
			if (found == trees_.end())
				return true;
			auto& tree = *found->second;
			const auto node = tree.document().find(drag.node);
			if (!node)
				return true;
			auto& prefs = *tree.at(*node).leaf.prefs;
			const double value = kinds::dragPrefsNumber(prefs, drag.item, drag.control, drag.track, drag.startX,
			                                            event.button.x * toOutputX, fonts_.size() / 15);
			prefs.dragValue.reset();
			prefs.dragItem.clear();
			prefs.width = -1;
			session_.sendEvent(
			    {{"ev", "change"}, {"sf", drag.surface}, {"id", drag.node}, {"item", drag.item}, {"value", value}});
			return true;
		}
		if (pointer_->cardPress) {
			pointer_->cardPress = false;
			return true;
		}
		if (event.button.button == SDL_BUTTON_RIGHT) {
			openMenu(event.button.x * toOutputX, event.button.y * toOutputY);
			return true;
		}
		if (event.button.button == SDL_BUTTON_LEFT)
			return click(event.button.x * toOutputX, event.button.y * toOutputY,
			             event.button.clicks >= 2 ? "dblclick" : "click");
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		menu_.clear();
		return wheel(event.wheel.mouse_x * toOutputX, event.wheel.mouse_y * toOutputY,
		             -event.wheel.y * fonts_.lineHeight() * 3)
		    || hadHover;
	case SDL_EVENT_TEXT_EDITING:
		if (active_)
			active_->get().setPreedit(event.edit.text ? event.edit.text : "", event.edit.start, event.edit.length);
		dismissHover();
		pointer_->suppressed = true;
		return true;
	case SDL_EVENT_TEXT_INPUT:
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		if (active_)
			active_->get().setPreedit({}, 0, 0);
		[[fallthrough]];
	case SDL_EVENT_KEY_DOWN: {
		dismissHover();
		pointer_->suppressed = true;
		const bool hadMenu = !menu_.empty();
		menu_.clear();
		return hadMenu || hadHover || event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_WINDOW_FOCUS_LOST;
	}
	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		dismissHover();
		if (systemPointer_)
			SDL_SetCursor(pointer_->arrow.get());
		pointer_->position = {-1, -1};
		pointer_->cursor = PointerState::Cursor::Arrow;
		return hadHover;
	default:
		break;
	}
	return false;
}

namespace {
// Shortcut text as the platform writes it.
std::string shortcut(char key) {
#ifdef __APPLE__
	return std::string("\xe2\x8c\x98") + key;
#else
	return std::string("Ctrl+") + key;
#endif
}
} // namespace

void Views::openMenu(float x, float y) {
	menu_.clear();
	for (auto it = painted_.rbegin(); it != painted_.rend(); ++it) {
		auto& tree = it->get();
		if (!tree.hit(x, y))
			continue;
		if (const auto editor = tree.editorAt(x, y); editor && session_.programFeature("edit")) {
			// Editing entries for the composer/input under the pointer.
			const auto& node = tree.document().at(*editor);
			const EditorRef ref{tree.surface().id, node.id};
			const bool selected =
			    hasEditorSelection() && editorSelection_->surface == ref.surface && editorSelection_->node == ref.node;
			const bool writable = !node.flag("readonly");
			menu_.push_back(
			    {MenuEntry::Kind::Cut, "Cut", shortcut('X'), ref.surface, ref.node, {}, selected && writable});
			menu_.push_back({MenuEntry::Kind::Copy, "Copy", shortcut('C'), ref.surface, ref.node, {}, selected});
			menu_.push_back({MenuEntry::Kind::Paste,
			                 "Paste",
			                 shortcut('V'),
			                 ref.surface,
			                 ref.node,
			                 {},
			                 writable && SDL_HasClipboardText()});
			menu_.push_back({MenuEntry::Kind::SelectAll,
			                 "Select all",
			                 shortcut('A'),
			                 ref.surface,
			                 ref.node,
			                 {},
			                 !node.str("text").empty()});
			break;
		}
		menu_.push_back({MenuEntry::Kind::Copy, "Copy", shortcut('C'), {}, {}, {}, selection_.has_value()});
		menu_.push_back({MenuEntry::Kind::SelectAll, "Select all", {}, {}, {}, {}, scrollOwner_.has_value()});
		for (const auto& [node, action] : tree.menuActions(x, y)) {
			// omp names actions in kebab/dotted form ("copy-message", "status.model"): show them as words.
			std::string label = action == "copy" ? "Copy text" : action;
			for (auto& ch : label)
				if (ch == '-' || ch == '_' || ch == '.')
					ch = ' ';
			if (!label.empty())
				label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
			menu_.push_back({MenuEntry::Kind::Action,
			                 std::move(label),
			                 {},
			                 tree.surface().id,
			                 tree.document().at(node).id,
			                 action,
			                 true});
		}
		break;
	}
	if (menu_.empty()) {
		menu_.push_back({MenuEntry::Kind::Copy, "Copy", shortcut('C'), {}, {}, {}, selection_.has_value()});
		menu_.push_back({MenuEntry::Kind::SelectAll, "Select all", {}, {}, {}, {}, scrollOwner_.has_value()});
	}
	const float em = fonts_.size(), row = fonts_.lineHeight() + em * 0.6f;
	float width = em * 10;
	for (const auto& entry : menu_)
		width = std::max(width, fonts_.width(text::FontId::Ui, entry.label)
		                            + fonts_.width(text::FontId::Ui, entry.shortcut) + em * 4);
	const float height = row * static_cast<float>(menu_.size()) + em * 0.6f;
	// Open at the pointer, flipped back inside the viewport when it would overflow.
	menuRect_ = {std::min(x, lastViewport_.x + lastViewport_.w - width),
	             std::min(y, lastViewport_.y + lastViewport_.h - height), width, height};
	menuReveal_.snap(0);
	menuReveal_.to(1, SDL_GetTicks(), 140);
	menuPaintRect_ = menuRect_;
	menuPaintRect_.y += em * 0.4f * (1 - menuReveal_.value(SDL_GetTicks()));
	menuClip_ = gfx::intersect(menuRect_, lastViewport_);
}

std::optional<size_t> Views::menuEntryAt(float x, float y) const {
	const float em = fonts_.size(), row = fonts_.lineHeight() + em * 0.6f, top = menuPaintRect_.y + em * 0.3f;
	if (!contains(menuClip_, x, y) || x < menuPaintRect_.x || x >= menuPaintRect_.x + menuPaintRect_.w || y < top
	    || y >= top + row * static_cast<float>(menu_.size()))
		return std::nullopt;
	return static_cast<size_t>((y - top) / row);
}

void Views::runMenuEntry(const MenuEntry& entry) {
	const bool editor = !entry.node.empty() && entry.kind != MenuEntry::Kind::Action;
	switch (entry.kind) {
	case MenuEntry::Kind::Cut:
		cutSelection();
		break;
	case MenuEntry::Kind::Copy:
		copySelection();
		break;
	case MenuEntry::Kind::Paste: {
		const auto target = resolve({entry.surface, entry.node});
		std::unique_ptr<char, decltype(&SDL_free)> clipboard(SDL_GetClipboardText(), SDL_free);
		if (!target || !clipboard || !*clipboard)
			break;
		std::string pasted(clipboard.get());
		std::erase(pasted, '\r');
		if (hasEditorSelection() && editorSelection_->node == entry.node)
			replaceEditorSelection(pasted);
		else {
			auto& [tree, id] = *target;
			const auto cursor = static_cast<size_t>(std::max(0.0, tree.get().document().at(id).num("cursor")));
			tree.get().sendEdit(id, cursor, cursor, pasted, session_);
		}
		break;
	}
	case MenuEntry::Kind::SelectAll:
		if (editor) {
			if (const auto target = resolve({entry.surface, entry.node}))
				selectEditor({entry.surface, entry.node},
				             std::pair{size_t{0}, text::utf16Length(
				                                      target->first.get().document().at(target->second).str("text"))});
		} else {
			// From the top-left of the transcript to past its end, in transcript coordinates.
			if (editorSelection_)
				selectEditor(*editorSelection_, std::nullopt);
			selection_.emplace(SDL_FPoint{mainClip_.x, 0},
			                   SDL_FPoint{mainClip_.x + mainClip_.w, contentHeight_ + mainClip_.h});
			selectWord_ = false;
		}
		break;
	case MenuEntry::Kind::Action:
		if (const auto found = trees_.find(entry.surface); found != trees_.end())
			if (const auto node = found->second->document().find(entry.node))
				found->second->runAction(*node, entry.action, session_);
		break;
	}
}

void Views::paintMenu() {
	if (menu_.empty())
		return;
	const float em = fonts_.size(), row = fonts_.lineHeight() + em * 0.6f;
	auto& renderer = renderer_.get();
	const float progress = menuReveal_.value(SDL_GetTicks());
	const bool moving = progress != 1;
	transitioning_ |= moving;
	menuPaintRect_ = menuRect_;
	menuPaintRect_.y += em * 0.4f * (1 - progress);
	menuClip_ = gfx::intersect(menuRect_, lastViewport_);
	const auto rect = menuPaintRect_;
	if (moving)
		clips_->push(menuRect_);
	gfx::rounded(renderer, {rect.x, rect.y + em * 0.2f, rect.w, rect.h}, em * 0.5f, text::rgb(0, 70));
	gfx::rounded(renderer, rect, em * 0.5f, text::palette::surface2);
	gfx::roundedBorder(renderer, rect, em * 0.5f, text::palette::border);
	const auto hovered = menuEntryAt(pointer_->position.x, pointer_->position.y);
	for (size_t index = 0; index < menu_.size(); ++index) {
		const auto& entry = menu_[index];
		const SDL_FRect item{rect.x + em * 0.3f, rect.y + em * 0.3f + row * static_cast<float>(index),
		                     rect.w - em * 0.6f, row};
		if (hovered == index && entry.enabled)
			gfx::rounded(renderer, item, em * 0.35f, text::palette::border);
		const text::Runs runs{{entry.label, {.color = entry.enabled ? text::palette::fg : text::palette::dim}}};
		const auto layout =
		    engine_.get().layout(runs, {.width = item.w - em * 1.4f, .wrap = text::Wrap::None, .maxLines = 1});
		engine_.get().draw(runs, layout, item.x + em * 0.7f, item.y + (row - layout.height) * 0.5f);
		if (entry.shortcut.empty())
			continue;
		const text::Runs keys{{entry.shortcut, {.color = text::palette::dim}}};
		const auto keysLayout = engine_.get().layout(keys, {.width = item.w, .wrap = text::Wrap::None, .maxLines = 1});
		engine_.get().draw(keys, keysLayout, item.x + item.w - em * 0.7f - keysLayout.width,
		                   item.y + (row - keysLayout.height) * 0.5f);
	}
	if (moving)
		clips_->pop();
}

std::optional<std::pair<std::reference_wrapper<Tree>, tsp::NodeId>> Views::resolve(const EditorRef& ref) {
	const auto found = trees_.find(ref.surface);
	if (found == trees_.end())
		return std::nullopt;
	const auto node = found->second->document().find(ref.node);
	if (!node)
		return std::nullopt;
	return std::pair{std::ref(*found->second), *node};
}

std::optional<std::pair<std::reference_wrapper<Tree>, tsp::NodeId>> Views::focusedEditor() {
	if (!active_)
		return std::nullopt;
	auto& tree = active_->get();
	const auto focus = tree.document().focus();
	if (!focus)
		return std::nullopt;
	const auto kind = tree.document().at(*focus).kind;
	if (kind != tsp::Kind::Editor && kind != tsp::Kind::Input)
		return std::nullopt;
	return std::pair{std::ref(tree), *focus};
}

bool Views::hasEditorSelection() const {
	if (!editorSelection_)
		return false;
	const auto found = trees_.find(editorSelection_->surface);
	if (found == trees_.end())
		return false;
	const auto node = found->second->document().find(editorSelection_->node);
	return node && found->second->at(*node).leaf.editorSelection.has_value();
}

void Views::selectEditor(const EditorRef& ref, std::optional<std::pair<size_t, size_t>> range) {
	if (editorSelection_ && (editorSelection_->surface != ref.surface || editorSelection_->node != ref.node))
		if (const auto previous = resolve(*editorSelection_))
			previous->first.get().setEditorSelection(previous->second, std::nullopt);
	if (const auto target = resolve(ref))
		target->first.get().setEditorSelection(target->second, range);
	if (range) {
		selection_.reset();
		editorSelection_ = ref;
	} else
		editorSelection_.reset();
}

std::string Views::editorSelectedText() {
	const auto target = editorSelection_ ? resolve(*editorSelection_) : std::nullopt;
	if (!target)
		return {};
	auto& [tree, id] = *target;
	const auto range = tree.get().at(id).leaf.editorSelection;
	if (!range)
		return {};
	const auto value = tree.get().document().at(id).str("text");
	const auto from = text::byteOffset(value, range->first), to = text::byteOffset(value, range->second);
	return std::string(value.substr(from, to - from));
}

void Views::replaceEditorSelection(std::string_view inserted) {
	const auto target = editorSelection_ ? resolve(*editorSelection_) : std::nullopt;
	if (!target)
		return;
	auto& [tree, id] = *target;
	if (const auto range = tree.get().at(id).leaf.editorSelection)
		tree.get().sendEdit(id, range->first, range->second, inserted, session_);
	selectEditor(*editorSelection_, std::nullopt);
}

void Views::cutSelection() {
	if (!hasEditorSelection())
		return;
	SDL_SetClipboardText(editorSelectedText().c_str());
	replaceEditorSelection({});
}

void Views::selectAll() {
	if (const auto editor = focusedEditor()) {
		auto& [tree, id] = *editor;
		const auto value = tree.get().document().at(id).str("text");
		if (!value.empty()) {
			selectEditor({tree.get().surface().id, tree.get().document().at(id).id},
			             std::pair{size_t{0}, text::utf16Length(value)});
			return;
		}
	}
	if (editorSelection_)
		selectEditor(*editorSelection_, std::nullopt);
	selection_.emplace(SDL_FPoint{mainClip_.x, 0}, SDL_FPoint{mainClip_.x + mainClip_.w, contentHeight_ + mainClip_.h});
	selectWord_ = false;
}

bool Views::editorKey(SDL_Keycode key, SDL_Keymod mod, std::string_view typed) {
	if (!hasEditorSelection())
		return false;
	const bool command = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT)) != 0;
	if (!command && !typed.empty()) {
		replaceEditorSelection(typed);
		return true;
	}
	if (!command && (key == SDLK_BACKSPACE || key == SDLK_DELETE)) {
		replaceEditorSelection({});
		return true;
	}
	if (!command && key >= SDLK_SPACE && key <= SDLK_TILDE)
		return false; // Its text follows as a text event.
	if ((mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && key == SDLK_V) {
		replaceEditorSelection({});
		return false;
	} // Paste lands there.
	selectEditor(*editorSelection_, std::nullopt);
	return false;
}

bool Tree::interactiveAt(float x, float y) const {
	const auto main = doc_.find("main");
	for (const auto& entry : targets_.entries()) {
		if (x < entry.rect.x || y < entry.rect.y || x >= entry.rect.x + entry.rect.w
		    || y >= entry.rect.y + entry.rect.h)
			continue;
		if (entry.kind == TargetKind::Hover)
			continue;
		if (entry.kind != TargetKind::Node || doc_.at(entry.node).prop("actions").is_object())
			return true;
		auto node = entry.node;
		while (node && (!main || node != *main))
			node = doc_.at(node).parent;
		if (!node)
			return true; // Dock and layer content is never transcript text.
	}
	return false;
}

bool Views::pointerCaptured() const {
	return pointer_->number.has_value() || selecting_ || editorDrag_.has_value() || asideDrag_.has_value();
}

std::string Views::selectionText() {
	if (hasEditorSelection())
		return editorSelectedText();
	if (!selection_)
		return {};
	// Text is gathered from the selected glyphs while painting; repaint the whole transcript
	// (into the unpresented backbuffer) so text scrolled out of view is included.
	gather_ = true;
	paint(lastViewport_);
	gather_ = false;
	return engine_.get().selectedText();
}

void Views::copySelection() {
	SDL_SetClipboardText(selectionText().c_str());
	selection_.reset(); // An editor range stays selected, as in any text field.
}

} // namespace view
