#include "view_tree.h"
#include "pointer.h"
#include "gfx/icons.h"
#include "kinds/chrome.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>

namespace view {
namespace {

// Width cap (in ems) of the centred transcript column and the composer dock that shares it.
constexpr float columnEms = 76;
// Width (ems) of the transcript minimap at the right edge.
constexpr float minimapEms = 6;

float space(std::string_view value, float em) {
	if (value == "xs")
		return em * gfx::space::xs;
	if (value == "sm")
		return em * gfx::space::sm;
	if (value == "md")
		return em * gfx::space::md;
	if (value == "lg")
		return em * gfx::space::xl;
	return 0;
}
YGAlign align(std::string_view value, bool row) {
	if (value == "center")
		return YGAlignCenter;
	if (value == "end")
		return YGAlignFlexEnd;
	if (value == "baseline")
		return YGAlignBaseline;
	if (value == "stretch")
		return YGAlignStretch;
	return row ? YGAlignFlexStart : YGAlignStretch;
}
bool fillsRow(const tsp::Node& node) {
	using enum tsp::Kind;
	switch (node.kind) {
	case Editor:
	case Input:
	case Md:
	case Code:
	case List:
	case Picker:
	case Progress:
	case Status:
		return true;
	case Meter:
		return node.str("style", "bar") == "bar";
	default:
		return false;
	}
}
SDL_Color tone(const tsp::Node& node, SDL_Color fallback) {
	text::RunStyle style{.color = fallback};
	text::applyTokens(node.str("tone"), style);
	return style.color;
}
bool contains(const SDL_FRect& rect, float x, float y) {
	return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}
// The prompt composer: `omp.editor`, or `omp.editor.bash`/`omp.editor.python` while the prompt
// starts with `!`/`$` (the same shell, with a mode label beside the input).
bool composer(std::string_view role) {
	return role == "omp.editor" || role == "omp.editor.bash" || role == "omp.editor.python";
}
bool hasPreview(const tsp::Node& node) {
	const auto& preview = node.prop("preview");
	return preview.is_object() || (node.kind == tsp::Kind::Card && preview == "auto")
	    || (node.kind == tsp::Kind::Tool && preview == "children");
}
// Width (ems) of the leading chevron column of a collapsible section.
constexpr float disclosureEms = 1.3f;
// The first line of a thinking section's Markdown body, without its emphasis/heading marks.
std::string thoughtExcerpt(const tsp::Document& doc, const tsp::Node& section) {
	for (const auto child : section.children) {
		const auto& body = doc.at(child);
		if (body.kind != tsp::Kind::Md)
			continue;
		const auto& text = body.str("text");
		size_t start = 0;
		while (start < text.size()) {
			const size_t end = std::min(text.find('\n', start), text.size());
			std::string line;
			for (size_t i = start; i < end; ++i)
				if (text[i] != '*' && text[i] != '`' && text[i] != '#' && text[i] != '>' && text[i] != '\r')
					line += text[i];
			const auto first = line.find_first_not_of(" \t_");
			if (first != std::string::npos)
				return line.substr(first);
			start = end + 1;
		}
	}
	return {};
}
void disclosure(SDL_Renderer& renderer, SDL_FPoint center, float em, bool collapsed) {
	const float d = em * 0.2f, stroke = em * 0.1f;
	if (collapsed) {
		gfx::line(renderer, {center.x - d, center.y - d}, center, text::palette::muted, stroke);
		gfx::line(renderer, center, {center.x - d, center.y + d}, text::palette::muted, stroke);
	} else {
		const SDL_FPoint low{center.x, center.y + d * 0.5f};
		gfx::line(renderer, {center.x - d, center.y - d * 0.5f}, low, text::palette::muted, stroke);
		gfx::line(renderer, low, {center.x + d, center.y - d * 0.5f}, text::palette::muted, stroke);
	}
}

} // namespace
void NodeView::snapScroll(float value) {
	scroll = value;
	scrollTween.snap(value);
}
void NodeView::sampleScroll(Uint64 now) {
	const float destination = std::clamp(scrollTween.target(), 0.0f, scrollMax);
	const float value = scrollTween.value(now), bounded = std::clamp(value, 0.0f, scrollMax);
	if (destination != scrollTween.target())
		snapScroll(destination);
	else if (value != bounded)
		snapScroll(bounded);
	else
		scroll = value;
}
void NodeView::wheelScroll(float delta, Uint64 now) {
	sampleScroll(now);
	scrollTween.to(std::clamp(scrollTween.target() + delta, 0.0f, scrollMax), now);
	scroll = scrollTween.value(now);
	stick = scrollTween.target() >= scrollMax - 2;
}

Tree::Tree(tsp::Surface& surface, text::Engine& engine, const text::Fonts& fonts,
           std::function<void(int)> runningChanged, ImageLookup images)
    : surface_(surface),
      doc_(*surface.doc),
      engine_(engine),
      fonts_(fonts),
      runningChanged_(std::move(runningChanged)),
      images_(std::move(images)) {
	doc_.attach({.added = [this](auto id) { added(id); },
	             .removing = [this](auto id) { removing(id); },
	             .moved = [this](auto id) { moved(id); },
	             .propsChanged =
	                 [this](auto id, const auto& changed) {
		                 if (changed.contains("collapsed"))
			                 at(id).collapsed.reset();
		                 // A terminal-side editor range no longer addresses the same text once the program changes it.
		                 if (changed.contains("text"))
			                 at(id).leaf.editorSelection.reset();
		                 sync(id);
		                 display(id);
	                 },
	             .textChanged =
	                 [this](auto id, size_t) {
		                 at(id).leaf.editorSelection.reset();
		                 textChanged(id);
	                 },
	             .focusChanged =
	                 [this](auto) {
		                 for (auto& node : nodes_)
			                 if (node
			                     && (doc_.at(node->id).kind == tsp::Kind::Editor
			                         || doc_.at(node->id).kind == tsp::Kind::Input)) {
				                 node->paintDirty = true;
				                 node->caret.reset();
				                 kinds::setPreedit(node->leaf.preedit, engine_.get(), {}, 0, 0);
			                 }
	                 }});
	added(doc_.root());
}
Tree::~Tree() {
	doc_.detach();
	for (auto& node : nodes_)
		if (node) {
			if (node->running && runningChanged_)
				runningChanged_(-1);
			node->leaf = {};
		}
}
void Tree::rebind(text::Engine& engine) {
	if (&engine_.get() == &engine)
		return;
	// Layouts contain no renderer resources and remain valid with the shared fonts.
	engine_.get().clear();
	engine_ = engine;
}
void Tree::dirtied(YGNodeConstRef node) {
	auto& view = *static_cast<NodeView*>(YGNodeGetContext(node));
	++view.tree.dirtyCount;
}
YGSize Tree::measure(YGNodeConstRef node, float width, YGMeasureMode widthMode, float height,
                     YGMeasureMode heightMode) {
	auto& view = *static_cast<NodeView*>(YGNodeGetContext(node));
	auto& tree = view.tree;
	++tree.measuredCount;
	view.leaf.expanded = tree.blockExpanded(view.id);
	view.leaf.budget = tree.budgetFor(view.id);
	auto size = kinds::measure(tree.doc_.at(view.id), view.leaf, tree.engine_.get(), tree.fonts_,
	                           widthMode == YGMeasureModeUndefined ? 1000000.0f : std::max(1.0f, width));
	if (widthMode == YGMeasureModeExactly)
		size.x = width;
	else if (widthMode == YGMeasureModeAtMost)
		size.x = std::min(size.x, width);
	if (heightMode == YGMeasureModeExactly)
		size.y = height;
	else if (heightMode == YGMeasureModeAtMost)
		size.y = std::min(size.y, height);
	return {size.x, size.y};
}
void Tree::added(tsp::NodeId id) {
	if (nodes_.size() <= id)
		nodes_.resize(static_cast<size_t>(id) + 1);
	nodes_[id] = std::make_unique<NodeView>(*this, id);
	++dirtyCount;
	auto& view = at(id);
	YGNodeSetContext(view.yoga.get(), &view);
	YGNodeSetDirtiedFunc(view.yoga.get(), dirtied);
	const auto& node = doc_.at(id);
	if (kinds::measured(node.kind) && node.children.empty())
		YGNodeSetMeasureFunc(view.yoga.get(), measure);
	sync(id);
	if (node.parent) {
		auto& parent = at(node.parent);
		// A newly materialised container may previously have been an empty text fallback.
		const bool wasMeasured = YGNodeHasMeasureFunc(parent.yoga.get());
		if (wasMeasured)
			YGNodeSetMeasureFunc(parent.yoga.get(), nullptr);
		const auto& siblings = doc_.at(node.parent).children;
		const auto index = static_cast<size_t>(std::find(siblings.begin(), siblings.end(), id) - siblings.begin());
		YGNodeInsertChild(parent.yoga.get(), view.yoga.get(), index);
		if (wasMeasured)
			sync(node.parent);
		const auto parentKind = doc_.at(node.parent).kind;
		if ((parentKind == tsp::Kind::Card || parentKind == tsp::Kind::Section)
		    && !doc_.at(node.parent).str("head").empty())
			sync(node.parent);
	}
	for (const auto child : node.children)
		added(child);
	display(id);
}
void Tree::removing(tsp::NodeId id) {
	if (hoveredTool_ == id)
		hoveredTool_ = 0;
	overview_.erase(id);
	for (auto parent = doc_.at(id).parent; parent; parent = doc_.at(parent).parent)
		if (at(parent).headerNode == id)
			at(parent).headerNode = 0;
	auto& view = at(id);
	if (const auto owner = YGNodeGetOwner(view.yoga.get()))
		YGNodeRemoveChild(owner, view.yoga.get());
	for (const auto child : doc_.at(id).children)
		removing(child);
	if (view.running) {
		view.running = false;
		if (runningChanged_)
			runningChanged_(-1);
	}
	view.leaf = {};
	nodes_[id].reset();
}
void Tree::moved(tsp::NodeId id) {
	auto& view = at(id);
	if (const auto owner = YGNodeGetOwner(view.yoga.get()))
		YGNodeRemoveChild(owner, view.yoga.get());
	const auto parent = doc_.at(id).parent;
	auto& parentView = at(parent);
	if (YGNodeHasMeasureFunc(parentView.yoga.get()))
		YGNodeSetMeasureFunc(parentView.yoga.get(), nullptr);
	const auto& siblings = doc_.at(parent).children;
	YGNodeInsertChild(parentView.yoga.get(), view.yoga.get(),
	                  static_cast<size_t>(std::find(siblings.begin(), siblings.end(), id) - siblings.begin()));
	syncSubtree(id);
	display(id);
}
bool Tree::nestedFrame(tsp::NodeId id) const {
	for (auto parent = doc_.at(id).parent; parent; parent = doc_.at(parent).parent) {
		const auto kind = doc_.at(parent).kind;
		if ((kind == tsp::Kind::Tool || kind == tsp::Kind::Card) && !at(parent).frameless)
			return true;
	}
	return false;
}
bool Tree::underComposer(tsp::NodeId id) const {
	const auto kind = doc_.at(id).kind;
	if ((kind == tsp::Kind::Tool || kind == tsp::Kind::Card) && nestedFrame(id))
		return true;
	for (auto parent = doc_.at(id).parent; parent; parent = doc_.at(parent).parent)
		if (composer(doc_.at(parent).str("role"))
		    || (kind == tsp::Kind::Code && doc_.at(parent).kind == tsp::Kind::Tool))
			return true;
	return false;
}
bool Tree::descendant(tsp::NodeId node, tsp::NodeId ancestor) const {
	for (auto current = node; current; current = doc_.at(current).parent)
		if (current == ancestor)
			return true;
	return false;
}
bool Tree::collapsed(tsp::NodeId id) const {
	return doc_.at(id).flag("collapsible") && at(id).collapsed.value_or(doc_.at(id).flag("collapsed"));
}
std::optional<tsp::NodeId> Tree::foldingAncestor(tsp::NodeId id) const {
	for (auto parent = doc_.at(id).parent; parent; parent = doc_.at(parent).parent)
		if (doc_.at(parent).flag("collapsible"))
			return parent;
	return std::nullopt;
}
bool Tree::blockExpanded(tsp::NodeId id) const {
	const auto& leaf = at(id).leaf;
	if (!leaf.code && !leaf.ansi && !leaf.rows)
		return leaf.expanded;
	const auto card = foldingAncestor(id);
	return card ? !collapsed(*card) : leaf.expanded;
}
float Tree::budgetFor(tsp::NodeId id) const {
	// An expanded block shows every line, whatever the card around it.
	if (at(id).leaf.expanded)
		return 0;
	for (auto parent = doc_.at(id).parent; parent; parent = doc_.at(parent).parent)
		if (at(parent).previewBudget > 0)
			return at(parent).previewBudget;
	return 0;
}
void Tree::remeasureLeaves(tsp::NodeId id) {
	// The first sync of a node runs before its children's views exist; they measure fresh anyway.
	for (const auto child : doc_.at(id).children) {
		if (child >= nodes_.size() || !nodes_[child])
			continue;
		if (YGNodeHasMeasureFunc(at(child).yoga.get()))
			YGNodeMarkDirty(at(child).yoga.get());
		remeasureLeaves(child);
	}
}
void Tree::display(tsp::NodeId id) {
	bool hidden = doc_.at(id).flag("hidden") || at(id).priorityHidden;
	const auto parent = doc_.at(id).parent;
	if (parent && doc_.at(parent).kind == tsp::Kind::List)
		hidden |= !kinds::listItemMatches(doc_.at(id), doc_.at(parent).str("filter"));
	for (auto parent = doc_.at(id).parent; parent; parent = doc_.at(parent).parent) {
		const auto heading = at(parent).headerNode;
		const bool headerBranch = heading && (descendant(id, heading) || descendant(heading, id));
		hidden |= collapsed(parent) && !hasPreview(doc_.at(parent)) && !headerBranch;
	}
	YGNodeStyleSetDisplay(at(id).yoga.get(), hidden ? YGDisplayNone : YGDisplayFlex);
	if (hidden)
		at(id).revealPaint = 0;
	for (const auto child : doc_.at(id).children)
		display(child);
}
void Tree::sync(tsp::NodeId id) {
	const auto& node = doc_.at(id);
	auto& view = at(id);
	const bool running = node.str("status") == "running";
	if (view.running != running) {
		view.running = running;
		if (runningChanged_)
			runningChanged_(running ? 1 : -1);
	}
	const auto yoga = view.yoga.get();
	const float em = fonts_.size(), pitch = fonts_.lineHeight();
	view.frameless =
	    (node.kind == tsp::Kind::Card && (node.str("variant") == "bare" || node.str("role") == "omp.welcome"))
	    || ((node.kind == tsp::Kind::Tool || node.kind == tsp::Kind::Card) && nestedFrame(id));
	if (node.kind == tsp::Kind::Image)
		view.leaf.image = images_ && !node.str("blob").empty() ? images_(node.str("blob")) : nullptr;
	kinds::sync(node, view.leaf, engine_.get(), fonts_, underComposer(id));
	const bool row = node.kind == tsp::Kind::Row || node.kind == tsp::Kind::Status;
	const bool inRow =
	    node.parent && (doc_.at(node.parent).kind == tsp::Kind::Row || doc_.at(node.parent).kind == tsp::Kind::Status);
	const bool fill = inRow && fillsRow(node) && node.prop("grow").is_null();
	YGNodeStyleSetFlexDirection(yoga, row ? YGFlexDirectionRow : YGFlexDirectionColumn);
	YGNodeStyleSetAlignItems(yoga, align(node.str("align"), row));
	YGNodeStyleSetAlignSelf(yoga, YGAlignAuto);
	YGNodeStyleSetJustifyContent(yoga, node.str("justify") == "between" ? YGJustifySpaceBetween
	                                   : node.str("justify") == "end"   ? YGJustifyFlexEnd
	                                                                    : YGJustifyFlexStart);
	YGNodeStyleSetFlexWrap(yoga, row && node.flag("wrap") ? YGWrapWrap : YGWrapNoWrap);
	YGNodeStyleSetGap(yoga, YGGutterAll, space(node.str("gap"), em));
	YGNodeStyleSetFlexGrow(yoga, static_cast<float>(node.num("grow", fill ? 1 : 0)));
	const bool textish = tsp::isTextKind(node.kind) || node.kind == tsp::Kind::Seg;
	const bool frameBody = node.parent
	                    && (doc_.at(node.parent).kind == tsp::Kind::Card || doc_.at(node.parent).kind == tsp::Kind::Tool
	                        || doc_.at(node.parent).kind == tsp::Kind::Agent);
	YGNodeStyleSetFlexShrink(yoga, static_cast<float>(node.num("shrink", !frameBody && (fill || textish) ? 1 : 0)));
	if (node.prop("basis").is_number())
		YGNodeStyleSetFlexBasisPercent(yoga, static_cast<float>(node.num("basis")) * 100);
	else if (fill)
		YGNodeStyleSetFlexBasis(yoga, 0);
	else
		YGNodeStyleSetFlexBasisAuto(yoga);
	YGNodeStyleSetWidthAuto(yoga);
	YGNodeStyleSetHeightAuto(yoga);
	YGNodeStyleSetMinWidth(yoga, YGUndefined);
	YGNodeStyleSetMinHeight(yoga, YGUndefined);
	YGNodeStyleSetMaxWidth(yoga, YGUndefined);
	YGNodeStyleSetMaxHeight(yoga, YGUndefined);
	for (const bool maximum : {false, true}) {
		const auto& bounds = node.prop(maximum ? "max" : "min");
		if (!bounds.is_object())
			continue;
		for (const bool height : {false, true}) {
			const auto value = bounds.find(height ? "h" : "w");
			if (value == bounds.end())
				continue;
			float amount = 0;
			const bool percent = value->is_number();
			if (percent)
				amount = value->get<float>() * 100;
			else if (value->is_string()) {
				const auto& extent = value->get_ref<const std::string&>();
				amount = std::strtof(extent.c_str(), nullptr) * (extent.ends_with("ch") ? fonts_.monoAdvance() : pitch);
			} else
				continue;
			if (maximum && height) {
				if (percent)
					YGNodeStyleSetMaxHeightPercent(yoga, amount);
				else
					YGNodeStyleSetMaxHeight(yoga, amount);
			} else if (maximum) {
				if (percent)
					YGNodeStyleSetMaxWidthPercent(yoga, amount);
				else
					YGNodeStyleSetMaxWidth(yoga, amount);
			} else if (height) {
				if (percent)
					YGNodeStyleSetMinHeightPercent(yoga, amount);
				else
					YGNodeStyleSetMinHeight(yoga, amount);
			} else {
				if (percent)
					YGNodeStyleSetMinWidthPercent(yoga, amount);
				else
					YGNodeStyleSetMinWidth(yoga, amount);
			}
		}
	}
	YGNodeStyleSetPadding(yoga, YGEdgeAll, 0);
	for (const auto edge : {YGEdgeLeft, YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeHorizontal, YGEdgeVertical})
		YGNodeStyleSetPadding(yoga, edge, YGUndefined);
	YGNodeStyleSetOverflow(yoga, YGOverflowVisible);
	YGNodeStyleSetPositionType(yoga, YGPositionTypeRelative);
	for (const auto edge : {YGEdgeLeft, YGEdgeTop, YGEdgeRight, YGEdgeBottom})
		YGNodeStyleSetPosition(yoga, edge, YGUndefined);
	YGNodeStyleSetMargin(yoga, YGEdgeLeft, 0);
	view.headerHeight = 0;
	view.headerNode = 0;
	view.header.clear();
	view.mark.clear();
	view.headerWidth = -1;
	if (node.kind == tsp::Kind::Tool || node.kind == tsp::Kind::Agent) {
		toolLayout(id, width_ > 0 ? width_ : 1000000.0f);
	} else if (node.kind == tsp::Kind::Card || node.kind == tsp::Kind::Section) {
		const bool card = node.kind == tsp::Kind::Card;
		const bool bare = card && view.frameless;
		const float pad = card && !bare ? em * gfx::space::lg : 0;
		text::RunStyle style{.font = card ? text::FontId::Bold : text::FontId::Ui,
		                     .color = card ? text::palette::fg : text::palette::muted};
		if (const auto heading = doc_.find(node.str("head")); heading && *heading != id && descendant(*heading, id))
			view.headerNode = *heading;
		else {
			view.header = text::toRuns(node.prop("head"), style);
			if (!card && view.header.empty() && node.flag("collapsible"))
				view.header = text::toRuns(collapsed(id) ? "Show" : "Hide", style);
			if (!card && node.prop("took").is_number())
				view.header.push_back(
				    {std::format("  {:.1f}s", node.num("took") / 1000), {.color = text::palette::dim}});
		}
		const auto status = node.str("status");
		if (!status.empty() && status != "running") {
			const std::string glyph = status == "done"      ? "✓"
			                        : status == "error"     ? "✗"
			                        : status == "cancelled" ? "⊘"
			                                                : "○";
			view.mark = {{glyph,
			              {.color = status == "error" ? text::palette::error
			                      : status == "done"  ? text::palette::success
			                                          : text::palette::muted}}};
			view.markLayout =
			    engine_.get().layout(view.mark, {.width = em * 1.3f, .wrap = text::Wrap::None, .maxLines = 1});
		}
		const bool header =
		    !view.headerNode && (!view.header.empty() || !node.str("status").empty() || node.flag("collapsible"));
		const bool bodyShown = !node.children.empty() && (!collapsed(id) || hasPreview(node));
		view.headerHeight = header ? pitch + (bodyShown ? em * gfx::space::sm : 0) : 0;
		if (!card && node.flag("collapsible")) {
			// Disclosure sections (thinking, compaction) lead with their chevron; the body lines up
			// with the heading text, the rule runs under the chevron.
			YGNodeStyleSetPadding(yoga, YGEdgeLeft, em * disclosureEms);
			YGNodeStyleSetPadding(yoga, YGEdgeRight, 0);
		} else {
			YGNodeStyleSetPadding(yoga, YGEdgeHorizontal, pad + (card ? 0 : em * gfx::space::md));
			if (view.headerNode && node.flag("collapsible"))
				YGNodeStyleSetPadding(yoga, YGEdgeRight, pad + em * 1.5f);
		}
		YGNodeStyleSetPadding(yoga, YGEdgeTop, pad + view.headerHeight);
		YGNodeStyleSetPadding(yoga, YGEdgeBottom, pad);
		YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::sm);
		if (collapsed(id) && hasPreview(node)) {
			const auto& preview = node.prop("preview");
			const float lines = preview.is_object() ? static_cast<float>(preview.value("lines", 6)) : 6.0f;
			YGNodeStyleSetMaxHeight(yoga, pad * 2 + view.headerHeight + lines * pitch);
			YGNodeStyleSetOverflow(yoga, YGOverflowHidden);
		}
	} else if (node.kind == tsp::Kind::Picker || node.kind == tsp::Kind::Prefs) {
		YGNodeStyleSetHeight(yoga, viewportHeight_ > 0 ? viewportHeight_ : pitch * 24);
		YGNodeStyleSetFlexShrink(yoga, 0);
	} else if (node.kind == tsp::Kind::Overlay) {
		view.header = text::toRuns(node.prop("head"), {.font = text::FontId::Bold});
		view.headerHeight = view.header.empty() ? 0 : pitch + em * gfx::space::sm;
		YGNodeStyleSetPadding(yoga, YGEdgeAll, em * gfx::space::lg);
		YGNodeStyleSetPadding(yoga, YGEdgeTop, em * gfx::space::lg + view.headerHeight);
	} else if (node.kind == tsp::Kind::List && !node.prop("max").is_null()) {
		const auto& max = node.prop("max");
		const float lines = max.is_object() ? max.value("lines", 8.0f) : max.is_number() ? max.get<float>() : 8.0f;
		YGNodeStyleSetMaxHeight(yoga, lines * (pitch + em * gfx::space::sm * 2));
		YGNodeStyleSetOverflow(yoga, YGOverflowScroll);
	} else if (node.kind == tsp::Kind::Col && composer(node.str("role"))) {
		YGNodeStyleSetPadding(yoga, YGEdgeHorizontal, em * gfx::space::lg);
		YGNodeStyleSetPadding(yoga, YGEdgeTop, pitch + em * (gfx::space::xs + gfx::space::lg));
		YGNodeStyleSetPadding(yoga, YGEdgeBottom, em * gfx::space::lg);
		YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::md);
		// HUD rows (notices, checklist pill) stack above the composer; keep them off its context meter.
		YGNodeStyleSetMargin(yoga, YGEdgeTop, em * gfx::space::sm);
	} else if (node.kind == tsp::Kind::Spacer) {
		YGNodeStyleSetHeight(yoga, space(node.str("size", "md"), em));
	} else if (node.kind == tsp::Kind::Status) {
		YGNodeStyleSetAlignItems(yoga, YGAlignCenter);
		YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::lg);
		YGNodeStyleSetMinHeight(yoga, pitch);
	}
	const auto role = node.str("role");
	if (role == "omp.welcome") {
		YGNodeStyleSetWidthPercent(yoga, 100);
		YGNodeStyleSetMaxWidth(yoga, em * 48);
		YGNodeStyleSetAlignSelf(yoga, YGAlignCenter);
		YGNodeStyleSetPadding(yoga, YGEdgeTop, em * 6);
		YGNodeStyleSetPadding(yoga, YGEdgeBottom, em * 2.5f);
		YGNodeStyleSetGap(yoga, YGGutterAll, em * 4);
	} else if (role == "omp.welcome.lockup" || role == "omp.welcome.tip") {
		YGNodeStyleSetAlignSelf(yoga, YGAlignCenter);
		YGNodeStyleSetMaxWidth(yoga, em * 48);
	} else if (role == "omp.composer.context") {
		YGNodeStyleSetPositionType(yoga, YGPositionTypeAbsolute);
		YGNodeStyleSetPosition(yoga, YGEdgeTop, 0);
		YGNodeStyleSetPosition(yoga, YGEdgeLeft, em * gfx::space::lg);
		YGNodeStyleSetPosition(yoga, YGEdgeRight, em * gfx::space::lg);
	} else if (role == "omp.composer.model" && !YGNodeHasMeasureFunc(yoga)) {
		YGNodeStyleSetPadding(yoga, YGEdgeHorizontal, em * gfx::space::md);
		YGNodeStyleSetPadding(yoga, YGEdgeVertical, em * gfx::space::xs);
		YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::sm);
	} else if (role == "omp.assistant" && node.prop("gap").is_null()) {
		// Thinking sections, text and tool rows of one reply; without a gap folded thoughts touch.
		YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::sm);
	} else if (role == "omp.composer.stop") {
		// Painted as a round stop button the size of the send button it replaces (kinds/inline.cpp).
		const float side = std::max(em * 2.2f, pitch + em * gfx::space::xs * 2);
		YGNodeStyleSetWidth(yoga, side);
		YGNodeStyleSetHeight(yoga, side);
		YGNodeStyleSetFlexShrink(yoga, 0);
	} else if (node.kind == tsp::Kind::Row && role.starts_with("omp.hud.")) {
		// Stacked HUD rows (progress notices, activity pills) above the composer need air between them.
		YGNodeStyleSetMargin(yoga, YGEdgeVertical, em * gfx::space::xs);
	} else if (role == "omp.hud.status" && node.prop("grow").is_null()) {
		// The working row inside spreads (its `fill` pushes Stop right), so the status column takes
		// the activity row's width and the todo pill ends it.
		YGNodeStyleSetFlexGrow(yoga, 1);
		YGNodeStyleSetFlexShrink(yoga, 1);
		YGNodeStyleSetFlexBasis(yoga, 0);
	} else if (role == "omp.job.tiles") {
		// omp asks for no gap between tiles (a terminal frames them); here they are columns with a
		// gutter and a divider (painted in paintNode), and wrapped rows a little apart.
		YGNodeStyleSetGap(yoga, YGGutterColumn, em * gfx::space::xl);
		YGNodeStyleSetGap(yoga, YGGutterRow, em * gfx::space::md);
	} else if (role == "omp.job.tile" && node.prop("grow").is_null()) {
		// Tiles share their (wrapping) row, so each head's id and clock sit at the tile's end.
		YGNodeStyleSetFlexGrow(yoga, 1);
		YGNodeStyleSetFlexBasis(yoga, em * 24);
	} else if (role == "omp.job.tile.lead") {
		// Under the title, past the status dot of the head row.
		YGNodeStyleSetMargin(yoga, YGEdgeLeft, fonts_.width(text::FontId::Ui, "✓") + em * gfx::space::xs);
	} else if (role == "omp.app.git" || role == "omp.app.git-narrow") {
		// omp's /git page (a screen surface): diff pane beside a changes sidebar, both full height.
		YGNodeStyleSetAlignItems(yoga, YGAlignStretch);
	} else if (role == "omp.app.git.main") {
		YGNodeStyleSetFlexGrow(yoga, 1);
		YGNodeStyleSetFlexShrink(yoga, 1);
		YGNodeStyleSetFlexBasis(yoga, 0);
		YGNodeStyleSetMinWidth(yoga, 0);
		YGNodeStyleSetMinHeight(yoga, 0);
		YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::sm);
		YGNodeStyleSetPadding(yoga, YGEdgeAll, em * gfx::space::md);
	} else if (role == "omp.app.git.side") {
		// Beside the diff, a quarter-ish of the width; below it (narrow page), part of the height.
		const bool below = node.parent && doc_.at(node.parent).str("role") == "omp.app.git-narrow";
		YGNodeStyleSetFlexShrink(yoga, 0);
		if (below)
			YGNodeStyleSetHeightPercent(yoga, 45);
		else {
			YGNodeStyleSetWidthPercent(yoga, 28);
			YGNodeStyleSetMinWidth(yoga, em * 18);
			YGNodeStyleSetMaxWidth(yoga, em * 30);
		}
		YGNodeStyleSetMinHeight(yoga, 0);
		YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::md);
		YGNodeStyleSetPadding(yoga, YGEdgeAll, em * gfx::space::md);
		YGNodeStyleSetOverflow(yoga, YGOverflowHidden);
	} else if (role == "omp.app.git.diff" || role == "omp.app.git.changes" || role == "omp.app.git.compare") {
		// The parts that take the remaining height and scroll inside it.
		YGNodeStyleSetFlexGrow(yoga, 1);
		YGNodeStyleSetFlexShrink(yoga, 1);
		YGNodeStyleSetFlexBasis(yoga, 0);
		YGNodeStyleSetMinHeight(yoga, 0);
		YGNodeStyleSetMinWidth(yoga, 0);
		YGNodeStyleSetOverflow(yoga, YGOverflowScroll);
	} else if (role == "omp.app.git.stat") {
		YGNodeStyleSetFlexShrink(yoga, 0);
	} else if (role == "omp.app.git.where") {
		YGNodeStyleSetFlexShrink(yoga, 1);
		YGNodeStyleSetMinWidth(yoga, 0);
		YGNodeStyleSetOverflow(yoga, YGOverflowHidden);
	} else if (role.starts_with("omp.app.git.dir-d") || role.starts_with("omp.app.git.file-d")) {
		// The changes tree: each level indents (the role carries the depth).
		const auto digits = role.substr(role.rfind("-d") + 2);
		int depth = 0;
		std::from_chars(digits.data(), digits.data() + digits.size(), depth);
		YGNodeStyleSetMargin(yoga, YGEdgeLeft, em * 0.9f * static_cast<float>(depth));
	} else if (role == "omp.tool.file") {
		// A search result's file line: icon, path, match-count badge.
		if (node.prop("align").is_null())
			YGNodeStyleSetAlignItems(yoga, YGAlignCenter);
		if (node.prop("gap").is_null())
			YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::sm);
	}
	// Floating toasts stay compact; one kept in the transcript reads as a notice across the column.
	if (node.kind == tsp::Kind::Toast && node.prop("max").is_null() && node.parent && doc_.at(node.parent).id != "main")
		YGNodeStyleSetMaxWidth(yoga, em * 30);
	if (node.parent && doc_.at(node.parent).kind == tsp::Kind::Status && node.str("side") == "right") {
		const auto& siblings = doc_.at(node.parent).children;
		const auto first = std::find_if(siblings.begin(), siblings.end(),
		                                [&](auto child) { return doc_.at(child).str("side") == "right"; });
		if (first != siblings.end() && *first == id)
			YGNodeStyleSetMarginAuto(yoga, YGEdgeLeft);
	}
	if (node.id == "main" && node.parent == doc_.root()) {
		YGNodeStyleSetOverflow(yoga, YGOverflowScroll);
		YGNodeStyleSetFlexShrink(yoga, 0);
		// One centred reading column, shared with the dock below; screens decide at layout, when
		// their children are known (see appPage()).
		if (surface_.mode != "screen") {
			YGNodeStyleSetWidthPercent(yoga, 100);
			YGNodeStyleSetMaxWidth(yoga, em * columnEms);
			YGNodeStyleSetAlignSelf(yoga, YGAlignCenter);
		}
		YGNodeStyleSetGap(yoga, YGGutterAll, em * gfx::space::md);
	}
	if (node.id == "dock" && node.parent == doc_.root()) {
		YGNodeStyleSetFlexShrink(yoga, 0);
		YGNodeStyleSetWidthPercent(yoga, 100);
		YGNodeStyleSetMaxWidth(yoga, em * columnEms);
		YGNodeStyleSetAlignSelf(yoga, YGAlignCenter);
	}
	if (node.id == "layer" && node.parent == doc_.root()) {
		YGNodeStyleSetPositionType(yoga, YGPositionTypeAbsolute);
		YGNodeStyleSetWidthPercent(yoga, 100);
	}
	{
		// Collapsed with a head-anchored preview: descendants need lay out only what fits in it
		// (a generous bound over the shown lines); a change re-measures them.
		const auto& preview = node.prop("preview");
		float budget = 0;
		if (collapsed(id) && hasPreview(node) && !(preview.is_object() && preview.contains("tail"))) {
			const float lines = preview.is_object() ? static_cast<float>(preview.value("lines", 6)) : 6.0f;
			budget = (lines + 2) * pitch * 1.5f;
		}
		if (budget != view.previewBudget) {
			view.previewBudget = budget;
			remeasureLeaves(id);
		}
	}
	if (YGNodeHasMeasureFunc(yoga))
		YGNodeMarkDirty(yoga);
	view.paintDirty = true;
}
void Tree::syncSubtree(tsp::NodeId id) {
	sync(id);
	for (const auto child : doc_.at(id).children)
		syncSubtree(child);
}
void Tree::blobArrived(std::string_view blob) {
	for (const auto& view : nodes_) {
		if (!view || doc_.at(view->id).kind != tsp::Kind::Image || doc_.at(view->id).str("blob") != blob)
			continue;
		sync(view->id);
		if (YGNodeHasMeasureFunc(view->yoga.get()))
			YGNodeMarkDirty(view->yoga.get());
	}
}
void Tree::textChanged(tsp::NodeId id) {
	auto& view = at(id);
	kinds::sync(doc_.at(id), view.leaf, engine_.get(), fonts_, underComposer(id));
	if (YGNodeHasMeasureFunc(view.yoga.get()))
		YGNodeMarkDirty(view.yoga.get());
	view.paintDirty = true;
	if (doc_.at(id).kind == tsp::Kind::Ansi && doc_.at(id).flag("follow")) {
		for (auto parent = doc_.at(id).parent; parent; parent = doc_.at(parent).parent) {
			auto& ancestor = at(parent);
			if (YGNodeStyleGetOverflow(ancestor.yoga.get()) != YGOverflowScroll)
				continue;
			if (ancestor.stick) {
				ancestor.snapScroll(std::numeric_limits<float>::max());
				if (doc_.at(parent).id == "main")
					doc_.reveals.emplace_back(doc_.at(id).id, "end");
			}
			break;
		}
	}
}
void Tree::toolLayout(tsp::NodeId id, float width) {
	const auto& node = doc_.at(id);
	auto& view = at(id);
	auto& cache = *view.leaf.toolAgent;
	kinds::measureToolAgent(node, cache, engine_.get(), fonts_, std::max(1.0f, width));
	if (node.children.empty())
		return;
	const auto yoga = view.yoga.get();
	// The head-to-body gap only exists while some child is shown (collapsed `preview:"none"` hides them all).
	// Read mounted Yoga children: the first sync runs before the children's views exist.
	bool bodyShown = false;
	for (size_t index = 0, count = YGNodeGetChildCount(yoga); index < count && !bodyShown; ++index)
		bodyShown = YGNodeStyleGetDisplay(YGNodeGetChild(yoga, index)) != YGDisplayNone;
	YGNodeStyleSetPadding(yoga, YGEdgeTop, cache.ownHeight - (bodyShown ? 0 : fonts_.size() * gfx::space::sm));
	YGNodeStyleSetPadding(yoga, YGEdgeBottom, cache.bottomInset);
	YGNodeStyleSetPadding(yoga, YGEdgeLeft, cache.childLeft);
	YGNodeStyleSetPadding(yoga, YGEdgeRight, cache.childRight);
	YGNodeStyleSetGap(yoga, YGGutterAll, fonts_.size() * gfx::space::sm);
	if (collapsed(id) && node.prop("preview").is_object()) {
		const auto& preview = node.prop("preview");
		const int lines = preview.contains("tail") ? preview.value("tail", 6) : preview.value("lines", 6);
		float inset = 0, lineHeight = fonts_.lineHeight();
		// Initial sync precedes mounting children; the layout pass supplies their measured insets.
		if (YGNodeGetChildCount(yoga) == node.children.size()) {
			const auto& edge = at(preview.contains("tail") ? node.children.back() : node.children.front()).leaf;
			inset = edge.padY;
			if (edge.code) {
				inset = edge.code->padY + (preview.contains("tail") ? 0 : edge.code->headerHeight);
				if (!edge.code->lines.empty() && edge.code->lines.front().layout.lineHeight > 0)
					lineHeight = edge.code->lines.front().layout.lineHeight;
			} else if (edge.ansi) {
				inset = edge.ansi->padY;
				if (edge.ansi->lineHeight > 0)
					lineHeight = edge.ansi->lineHeight;
			}
		}
		YGNodeStyleSetMaxHeight(yoga, cache.ownHeight + cache.bottomInset + inset
		                                  + static_cast<float>(std::max(0, lines)) * lineHeight);
		YGNodeStyleSetOverflow(yoga, YGOverflowHidden);
	}
}
void Tree::statusLayout(tsp::NodeId id, float width) {
	const auto& node = doc_.at(id);
	const float gap = fonts_.size() * gfx::space::lg;
	struct Segment {
		tsp::NodeId id;
		float width, height;
		bool right;
	};
	std::vector<Segment> segments;
	segments.reserve(node.children.size());
	float required = 0, height = fonts_.lineHeight();
	for (const auto child : node.children) {
		if (doc_.at(child).flag("hidden"))
			continue;
		auto& view = at(child);
		float natural = view.leaf.naturalWidth > 0 ? view.leaf.naturalWidth : YGNodeLayoutGetWidth(view.yoga.get());
		const auto minimum = YGNodeStyleGetMinWidth(view.yoga.get());
		if (minimum.unit == YGUnitPoint)
			natural = std::max(natural, minimum.value);
		else if (minimum.unit == YGUnitPercent)
			natural = std::max(natural, width * minimum.value * 0.01f);
		const float childHeight = YGNodeHasMeasureFunc(view.yoga.get())
		                            ? kinds::measure(doc_.at(child), view.leaf, engine_.get(), fonts_, natural).y
		                            : YGNodeLayoutGetHeight(view.yoga.get());
		height = std::max(height, childHeight);
		segments.push_back({child, natural, childHeight, doc_.at(child).str("side") == "right"});
		required += natural + gap;
		view.priorityHidden = false;
	}
	YGNodeStyleSetMinHeight(at(id).yoga.get(), height);
	if (!segments.empty())
		required -= gap;
	std::vector<size_t> priority;
	priority.reserve(segments.size());
	for (size_t i = 0; i < segments.size(); ++i)
		priority.push_back(i);
	std::stable_sort(priority.begin(), priority.end(), [&](size_t a, size_t b) {
		return doc_.at(segments[a].id).num("priority") < doc_.at(segments[b].id).num("priority");
	});
	for (const auto index : priority) {
		if (required <= width)
			break;
		const auto& segment = segments[index];
		at(segment.id).priorityHidden = true;
		required -= segment.width + gap;
	}
	float rightWidth = 0;
	for (const auto& segment : segments)
		if (!at(segment.id).priorityHidden && segment.right)
			rightWidth += segment.width + gap;
	if (rightWidth > 0)
		rightWidth -= gap;
	float left = 0, right = width - rightWidth;
	for (const auto& segment : segments) {
		auto& view = at(segment.id);
		display(segment.id);
		const auto yoga = view.yoga.get();
		YGNodeStyleSetPositionType(yoga, YGPositionTypeAbsolute);
		YGNodeStyleSetFlexShrink(yoga, 0);
		YGNodeStyleSetWidth(yoga, segment.width);
		YGNodeStyleSetMargin(yoga, YGEdgeLeft, 0);
		if (view.priorityHidden)
			continue;
		float& x = segment.right ? right : left;
		YGNodeStyleSetPosition(yoga, YGEdgeLeft, x);
		YGNodeStyleSetPosition(yoga, YGEdgeTop, (height - segment.height) * 0.5f);
		x += segment.width + gap;
	}
}
void Tree::forgetLeaves() {
	for (auto& view : nodes_) {
		if (!view)
			continue;
		const bool expanded = view->leaf.expanded;
		view->leaf = {};
		view->leaf.expanded = expanded;
	}
}
void Tree::metricsChanged() {
	syncSubtree(doc_.root());
	display(doc_.root());
	width_ = -1;
}
void Tree::listLayout(tsp::NodeId id, float width) {
	auto& view = at(id);
	const auto& node = doc_.at(id);
	view.listEmpty = std::none_of(node.children.begin(), node.children.end(), [&](auto child) {
		return YGNodeStyleGetDisplay(at(child).yoga.get()) != YGDisplayNone;
	});
	if (!node.children.empty()) {
		const float height =
		    view.listEmpty ? kinds::measureListTabs(node, *view.leaf.listTabs, engine_.get(), fonts_, width).y : 0;
		YGNodeStyleSetPadding(view.yoga.get(), YGEdgeTop, height);
		if (!node.prop("max").is_null() && !view.listEmpty) {
			const auto& max = node.prop("max");
			float rows = max.is_object() ? max.value("lines", 8.0f) : max.is_number() ? max.get<float>() : 8.0f;
			const float gap = space(node.str("gap"), fonts_.size());
			float height = 0;
			bool first = true;
			// A line limit counts padded rows, including their keycaps, not bare font pitches.
			for (const auto child : node.children) {
				const auto yoga = at(child).yoga.get();
				if (YGNodeStyleGetDisplay(yoga) == YGDisplayNone)
					continue;
				if (rows <= 0)
					break;
				if (!first)
					height += gap;
				height += YGNodeLayoutGetHeight(yoga) * std::min(1.0f, rows);
				rows -= 1;
				first = false;
			}
			YGNodeStyleSetMaxHeight(view.yoga.get(), height);
		}
	}
}

void Tree::layout(float width, float viewportHeight) {
	laidOut = width_ != width || viewportHeight_ != viewportHeight || YGNodeIsDirty(at(doc_.root()).yoga.get());
	if (!laidOut)
		return;
	viewportHeight_ = viewportHeight;
	if (viewportHeight > 0)
		for (const auto& view : nodes_)
			if (view) {
				const auto& node = doc_.at(view->id);
				if ((node.kind == tsp::Kind::Picker || node.kind == tsp::Kind::Prefs) && node.parent
				    && doc_.at(node.parent).id == "main")
					YGNodeStyleSetHeight(view->yoga.get(), std::max(1.0f, viewportHeight));
				// An app page: main is the viewport's height and full width, and the page fills it,
				// scrolling its own panes. Other screens (the rewind page) get the reading column and
				// scroll main like a transcript.
				if (surface_.mode == "screen" && node.id == "main" && node.parent == doc_.root()) {
					const auto yoga = view->yoga.get();
					if (appPage()) {
						YGNodeStyleSetHeight(yoga, std::max(1.0f, viewportHeight));
						YGNodeStyleSetWidthAuto(yoga);
						YGNodeStyleSetMaxWidth(yoga, YGUndefined);
						YGNodeStyleSetAlignSelf(yoga, YGAlignAuto);
					} else {
						YGNodeStyleSetHeightAuto(yoga);
						YGNodeStyleSetWidthPercent(yoga, 100);
						YGNodeStyleSetMaxWidth(yoga, fonts_.size() * columnEms);
						YGNodeStyleSetAlignSelf(yoga, YGAlignCenter);
					}
				}
				if (surface_.mode == "screen" && node.parent && doc_.at(node.parent).id == "main"
				    && node.str("role").starts_with("omp.app")) {
					YGNodeStyleSetFlexGrow(view->yoga.get(), 1);
					YGNodeStyleSetFlexBasis(view->yoga.get(), 0);
					YGNodeStyleSetMinHeight(view->yoga.get(), 0);
				}
			}
	lastDirty = dirtyCount;
	measuredCount = 0;
	YGNodeCalculateLayout(at(doc_.root()).yoga.get(), width, YGUndefined, YGDirectionLTR);
	for (const auto& view : nodes_)
		if (view) {
			const auto& node = doc_.at(view->id);
			if (node.kind == tsp::Kind::Tool || node.kind == tsp::Kind::Agent)
				toolLayout(view->id, YGNodeLayoutGetWidth(view->yoga.get()));
			else if (node.kind == tsp::Kind::Status)
				statusLayout(view->id, YGNodeLayoutGetWidth(view->yoga.get()));
			else if (node.kind == tsp::Kind::List)
				listLayout(view->id, YGNodeLayoutGetWidth(view->yoga.get()));
		}
	if (YGNodeIsDirty(at(doc_.root()).yoga.get()))
		YGNodeCalculateLayout(at(doc_.root()).yoga.get(), width, YGUndefined, YGDirectionLTR);
	lastMeasured = measuredCount;
	dirtyCount = 0;
	width_ = width;
}
float Tree::height(std::string_view region) const {
	const auto id = doc_.find(region);
	return id ? YGNodeLayoutGetHeight(at(*id).yoga.get()) : 0;
}
void Tree::beginPaint(bool partial, bool more, bool transitions) {
	targets_.freeze(partial);
	partial_ = partial;
	if (more)
		return; // Another damage region of the same frame: keep what earlier regions recorded.
	animated.clear();
	relayout = false;
	transitioning = false;
	if (!partial) {
		++paintSerial_;
		transitionsReady_ = transitions;
	}
	// Every leaf that schedules a wake-up is an animation area, so a partial paint repaints all
	// of them and rebuilds the wake time; hit targets and the caret come from the last full paint.
	nextWake = 0;
	if (partial)
		return;
	scrollers_.clear();
	targets_.clear();
	if (const auto focus = doc_.focus())
		at(*focus).caret.reset();
}
float Tree::reveal(NodeView& view, Uint64 duration) {
	if (view.revealPaint == paintSerial_)
		return view.revealValue;
	const Uint64 now = SDL_GetTicks();
	if (!view.revealPaint || view.revealPaint + 1 < paintSerial_) {
		view.reveal.snap(transitionsReady_ ? 0.0f : 1.0f);
		view.reveal.to(1, now, duration);
	}
	view.revealPaint = paintSerial_;
	view.revealValue = view.reveal.value(now);
	transitioning |= view.revealValue != 1;
	return view.revealValue;
}
bool Tree::paintNode(tsp::NodeId id, float parentX, float parentY, SDL_Renderer& renderer, gfx::ClipStack& clips) {
	auto& view = at(id);
	const auto& node = doc_.at(id);
	const auto yoga = view.yoga.get();
	if (YGNodeStyleGetDisplay(yoga) == YGDisplayNone)
		return false;
	view.rect = {parentX + YGNodeLayoutGetLeft(yoga), parentY + YGNodeLayoutGetTop(yoga), YGNodeLayoutGetWidth(yoga),
	             YGNodeLayoutGetHeight(yoga)};
	if (!gfx::intersects(view.rect, clips.current()))
		return false;
	targets_.bind(id, clips.current());
	targets_.add(view.rect, TargetKind::Node);
	view.hitRect = gfx::intersect(view.rect, clips.current());
	const auto rect = view.rect;
	const float em = fonts_.size(), pitch = fonts_.lineHeight(), scale = em / 15;
	if (node.kind == tsp::Kind::Picker) {
		auto sheet = rect;
		sheet.y -= view.scroll;
		auto& cache = *view.leaf.picker;
		targets_.bind(id, gfx::intersect(rect, clips.current()));
		bool animating = kinds::paintPicker(node, cache, engine_.get(), fonts_, renderer, sheet,
		                                    doc_.focus() == id || !doc_.focus(), targets_);
		transitioning |= cache.transitioning;
		if (animating || cache.nextWake >= 0)
			animated.push_back(gfx::intersect(rect, clips.current()));
		view.caret = cache.caret;
		if (cache.nextWake >= 0)
			wakeAt(SDL_GetTicks() + static_cast<Uint64>(cache.nextWake));
		auto preview = cache.previewBox;
		preview.x += sheet.x;
		preview.y += sheet.y;
		if (preview.w > 0 && preview.h > 0) {
			float contentHeight = 0;
			for (const auto child : node.children) {
				const auto childYoga = at(child).yoga.get();
				YGNodeStyleSetWidth(childYoga, preview.w);
				if (laidOut || at(child).paintDirty || YGNodeIsDirty(childYoga)
				    || YGNodeLayoutGetWidth(childYoga) != preview.w)
					YGNodeCalculateLayout(childYoga, preview.w, YGUndefined, YGDirectionLTR);
				contentHeight += YGNodeLayoutGetHeight(childYoga) + em * gfx::space::sm;
			}
			cache.previewMax = std::max(0.0f, contentHeight - preview.h);
			const float destination = std::clamp(cache.previewScrollTween.target(), 0.0f, cache.previewMax);
			if (destination != cache.previewScrollTween.target())
				cache.previewScrollTween.snap(destination);
			cache.previewScroll = cache.previewScrollTween.value(SDL_GetTicks());
			const float bounded = std::clamp(cache.previewScroll, 0.0f, cache.previewMax);
			if (cache.previewScroll != bounded) {
				cache.previewScrollTween.snap(bounded);
				cache.previewScroll = bounded;
			}
			clips.push(preview);
			float y = preview.y - cache.previewScroll;
			for (const auto child : node.children) {
				const auto childYoga = at(child).yoga.get();
				animating |= paintNode(child, preview.x - YGNodeLayoutGetLeft(childYoga),
				                       y - YGNodeLayoutGetTop(childYoga), renderer, clips);
				y += YGNodeLayoutGetHeight(childYoga) + em * gfx::space::sm;
			}
			clips.pop();
			if (cache.previewMax > 0) {
				const float thumb = std::max(em, preview.h * preview.h / contentHeight);
				gfx::rounded(renderer,
				             {preview.x + preview.w - 3 * scale,
				              preview.y + (preview.h - thumb) * cache.previewScroll / cache.previewMax, 2 * scale,
				              thumb},
				             scale, text::palette::dim);
			}
		}
		view.paintDirty = false;
		return animating;
	}
	if (node.kind == tsp::Kind::Prefs) {
		auto& cache = *view.leaf.prefs;
		float statusHeight = 0;
		for (const auto child : node.children)
			if (doc_.at(child).str("role") == "omp.prefs.preview.status") {
				const auto childYoga = at(child).yoga.get();
				const float width = std::max(1.0f, rect.w - em * 2);
				YGNodeStyleSetWidth(childYoga, width);
				if (laidOut || at(child).paintDirty || YGNodeIsDirty(childYoga)
				    || YGNodeLayoutGetWidth(childYoga) != width)
					YGNodeCalculateLayout(childYoga, width, YGUndefined, YGDirectionLTR);
				statusHeight = YGNodeLayoutGetHeight(childYoga);
			}
		if (cache.previewHeight != statusHeight) {
			cache.previewHeight = statusHeight;
			cache.width = -1;
		}
		targets_.bind(id, gfx::intersect(rect, clips.current()));
		view.caret = kinds::paintPrefs(node, cache, engine_.get(), fonts_, renderer, rect,
		                               doc_.focus() == id || !doc_.focus(), targets_);
		transitioning |= cache.transitioning;
		bool animating = false;
		clips.push(cache.bodyRect);
		for (const auto child : node.children) {
			const auto childYoga = at(child).yoga.get();
			const auto role = doc_.at(child).str("role");
			if (role == "omp.prefs.preview.status" && cache.previewBox.h > 0) {
				animating |=
				    paintNode(child, rect.x + cache.previewBox.x - YGNodeLayoutGetLeft(childYoga),
				              cache.bodyRect.y + cache.previewBox.y - cache.scroll - YGNodeLayoutGetTop(childYoga),
				              renderer, clips);
			} else if (role == "omp.prefs.editor") {
				const float width = std::max(1.0f, cache.bodyRect.w - em * 2);
				YGNodeStyleSetWidth(childYoga, width);
				if (laidOut || at(child).paintDirty || YGNodeIsDirty(childYoga)
				    || YGNodeLayoutGetWidth(childYoga) != width)
					YGNodeCalculateLayout(childYoga, width, YGUndefined, YGDirectionLTR);
				const float height = std::min(cache.bodyRect.h, YGNodeLayoutGetHeight(childYoga));
				targets_.bind(id, cache.bodyRect);
				targets_.add(cache.bodyRect, TargetKind::Block);
				animating |=
				    paintNode(child, cache.bodyRect.x + em - YGNodeLayoutGetLeft(childYoga),
				              cache.bodyRect.y + (cache.bodyRect.h - height) * 0.5f - YGNodeLayoutGetTop(childYoga),
				              renderer, clips);
			} else if (doc_.at(child).kind == tsp::Kind::Picker && !doc_.at(child).flag("hidden")) {
				// A choice with too many options for a menu (themes) opens as a picker sheet over
				// the page; the page under it stays dimmed and inert until it closes.
				auto& picker = *at(child).leaf.picker;
				const float inset = em * gfx::space::sm;
				const float width = std::max(1.0f, cache.bodyRect.w - inset * 2);
				const float height = std::max(1.0f, cache.bodyRect.h - inset * 2);
				kinds::layoutPicker(doc_.at(child), picker, engine_.get(), fonts_, width, height);
				YGNodeStyleSetWidth(childYoga, width);
				YGNodeStyleSetHeight(childYoga, std::max(height, picker.minimumHeight));
				if (laidOut || at(child).paintDirty || YGNodeIsDirty(childYoga)
				    || YGNodeLayoutGetWidth(childYoga) != width
				    || YGNodeLayoutGetHeight(childYoga) != std::max(height, picker.minimumHeight))
					YGNodeCalculateLayout(childYoga, width, YGUndefined, YGDirectionLTR);
				gfx::fill(renderer, rect, text::rgb(0, 110));
				targets_.bind(id, rect);
				targets_.add(rect, TargetKind::Block);
				const float progress = reveal(at(child));
				const SDL_FRect box{cache.bodyRect.x + inset, cache.bodyRect.y + inset, width, height};
				if (progress != 1)
					clips.push(box);
				animating |=
				    paintNode(child, box.x - YGNodeLayoutGetLeft(childYoga),
				              box.y + em * 0.6f * (1 - progress) - YGNodeLayoutGetTop(childYoga), renderer, clips);
				if (progress != 1)
					clips.pop();
			}
		}
		clips.pop();
		view.paintDirty = false;
		return animating;
	}
	if (node.kind == tsp::Kind::Item && node.parent && doc_.at(node.parent).kind == tsp::Kind::List) {
		const auto selected = doc_.at(node.parent).str("selected");
		if (selected == node.id || (!node.str("key").empty() && selected == node.str("key")))
			gfx::rounded(renderer, rect, em * 0.3f, text::palette::surface2);
	}
	if (node.flag("collapsible"))
		targets_.add({rect.x, rect.y, rect.w,
		              view.headerHeight > 0 ? view.headerHeight + em * 0.5f : std::min(rect.h, pitch + em * 0.6f)},
		             TargetKind::Toggle);
	if (node.parent && node.str("mark") == "pick") {
		const auto& siblings = doc_.at(node.parent).children;
		const auto position = std::find(siblings.begin(), siblings.end(), id);
		const bool previousPick = position != siblings.begin() && doc_.at(*std::prev(position)).str("mark") == "pick";
		const bool nextPick =
		    std::next(position) != siblings.end() && doc_.at(*std::next(position)).str("mark") == "pick";
		kinds::paintPick(renderer, rect, em, scale, previousPick, nextPick);
	}
	// A job tile after another on the same row: a divider in the middle of the gutter.
	if (node.str("role") == "omp.job.tile" && YGNodeLayoutGetLeft(yoga) > 0.5f) {
		const float x = std::round(rect.x - em * gfx::space::xl * 0.5f);
		gfx::fill(renderer, {x, rect.y, std::max(1.0f, scale), rect.h}, text::palette::border);
	}
	bool animating = false;
	if (node.kind == tsp::Kind::Card && !view.frameless) {
		gfx::rounded(renderer, rect, 11 * scale, kinds::cardFill(node));
		gfx::roundedBorder(renderer, rect, 11 * scale, kinds::cardBorder(node), scale);
	} else if (node.kind == tsp::Kind::Col && composer(node.str("role"))) {
		const float context = pitch + em * gfx::space::xs;
		const SDL_FRect box{rect.x, rect.y + context, rect.w, std::max(0.0f, rect.h - context)};
		gfx::rounded(renderer, box, 12 * scale, text::palette::surface);
		gfx::roundedBorder(renderer, box, 12 * scale, text::palette::border, scale);
	} else if (node.str("role") == "omp.composer.model") {
		gfx::rounded(renderer, rect, rect.h * 0.5f, text::palette::bg);
		gfx::roundedBorder(renderer, rect, rect.h * 0.5f, text::palette::border, scale);
	} else if (node.kind == tsp::Kind::Status && !node.flag("transparent"))
		gfx::fill(renderer, rect, text::palette::surface);
	if (view.headerHeight > 0) {
		const bool card = node.kind == tsp::Kind::Card;
		const float pad =
		    node.kind == tsp::Kind::Overlay || node.kind == tsp::Kind::Picker || node.kind == tsp::Kind::Prefs
		        ? em * gfx::space::lg
		    : card && !view.frameless ? em * gfx::space::lg
		                              : 0;
		float x = rect.x + pad, y = rect.y + pad;
		const bool leading = node.kind == tsp::Kind::Section && node.flag("collapsible");
		if (leading)
			x += em * disclosureEms;
		const auto status = node.str("status");
		const float chevron = node.flag("collapsible") && !leading ? em * 1.5f : 0;
		if (!status.empty()) {
			if (status == "running") {
				const float start = static_cast<float>(SDL_GetTicks() % 1200) * 6.2831853f / 1200;
				gfx::arc(renderer, {x + em * 0.5f, y + pitch * 0.5f}, em * 0.35f, start, start + 4.7f,
				         text::palette::accent, 1.5f * scale);
				animating = true;
			} else
				engine_.get().draw(view.mark, view.markLayout, x, y);
			x += em * 1.3f;
		}
		const float width = std::max(1.0f, rect.x + rect.w - pad - chevron - x);
		if (view.headerWidth != width) {
			view.headerLayout =
			    engine_.get().layout(view.header, {.width = width, .wrap = text::Wrap::None, .maxLines = 1});
			view.headerWidth = width;
		}
		engine_.get().draw(view.header, view.headerLayout, x, y);
		if (node.kind == tsp::Kind::Overlay)
			gfx::line(renderer, {rect.x + pad, rect.y + pad + view.headerHeight - em * gfx::space::xs},
			          {rect.x + rect.w - pad, rect.y + pad + view.headerHeight - em * gfx::space::xs},
			          text::palette::border, scale);
		if (node.flag("collapsible"))
			disclosure(
			    renderer,
			    {leading ? rect.x + em * disclosureEms * 0.4f : rect.x + rect.w - pad - em * 0.5f, y + pitch * 0.5f},
			    em, collapsed(id));
	}
	if (node.kind == tsp::Kind::Section && !node.children.empty() && !collapsed(id)) {
		float headerBottom = view.headerHeight;
		if (view.headerNode) {
			headerBottom = YGNodeLayoutGetHeight(at(view.headerNode).yoga.get());
			for (auto heading = view.headerNode; heading != id; heading = doc_.at(heading).parent)
				headerBottom += YGNodeLayoutGetTop(at(heading).yoga.get());
		}
		const float ruleX = rect.x + em * (node.flag("collapsible") ? disclosureEms * 0.4f : 0.25f);
		gfx::line(renderer, {ruleX, rect.y + headerBottom + em * gfx::space::xs}, {ruleX, rect.y + rect.h},
		          text::palette::border, scale);
	}
	const bool clip = YGNodeStyleGetOverflow(yoga) != YGOverflowVisible && node.id != "main";
	// Lists and overflow:scroll boxes (an app page's panes) scroll their children; a measured
	// leaf set to scroll (a diff pane) scrolls its own content.
	const bool scrollBox =
	    node.kind == tsp::Kind::List
	    || (YGNodeStyleGetOverflow(yoga) == YGOverflowScroll && node.id != "main" && !YGNodeHasMeasureFunc(yoga));
	const bool scrollLeaf = YGNodeStyleGetOverflow(yoga) == YGOverflowScroll && YGNodeHasMeasureFunc(yoga);
	if (scrollBox) {
		float bottom = 0;
		for (auto child : node.children)
			if (YGNodeStyleGetDisplay(at(child).yoga.get()) != YGDisplayNone)
				bottom = std::max(bottom, YGNodeLayoutGetTop(at(child).yoga.get())
				                              + YGNodeLayoutGetHeight(at(child).yoga.get()));
		view.scrollMax = std::max(0.0f, bottom + YGNodeLayoutGetPadding(yoga, YGEdgeBottom) - rect.h);
		view.sampleScroll(SDL_GetTicks());
		transitioning |= view.scroll != view.scrollTween.target();
	}
	if (node.kind == tsp::Kind::List) {
		if (view.selected != node.str("selected")) {
			view.selected = node.str("selected");
			const auto selected = std::find_if(node.children.begin(), node.children.end(), [&](auto child) {
				return doc_.at(child).id == view.selected
				    || (!doc_.at(child).str("key").empty() && doc_.at(child).str("key") == view.selected);
			});
			if (selected != node.children.end() && YGNodeStyleGetDisplay(at(*selected).yoga.get()) != YGDisplayNone) {
				const auto child = at(*selected).yoga.get();
				const float top = YGNodeLayoutGetTop(child), end = top + YGNodeLayoutGetHeight(child);
				if (top < view.scroll)
					view.scroll = top;
				if (end > view.scroll + rect.h)
					view.scroll = end - rect.h;
			}
			view.snapScroll(std::clamp(view.scroll, 0.0f, view.scrollMax));
		}
	}
	if ((scrollBox || scrollLeaf) && !partial_)
		scrollers_.push_back({gfx::intersect(rect, clips.current()), id});
	if (clip)
		clips.push(rect);
	if (YGNodeHasMeasureFunc(yoga) || node.kind == tsp::Kind::Tool || node.kind == tsp::Kind::Agent
	    || (node.kind == tsp::Kind::List && view.listEmpty)) {
		targets_.bind(id, gfx::intersect(rect, clips.current()));
		view.leaf.collapsed = collapsed(id);
		view.leaf.expanded = blockExpanded(id);
		view.leaf.budget = budgetFor(id);
		auto leafRect = rect;
		if (scrollLeaf) {
			view.leaf.budget = 0;
			const float natural = kinds::measure(node, view.leaf, engine_.get(), fonts_, rect.w).y;
			view.scrollMax = std::max(0.0f, natural - rect.h);
			view.sampleScroll(SDL_GetTicks());
			transitioning |= view.scroll != view.scrollTween.target();
			leafRect = {rect.x, rect.y - view.scroll, rect.w, std::max(rect.h, natural)};
		}
		const auto result =
		    kinds::paint(node, view.leaf, engine_.get(), fonts_, renderer, leafRect, doc_.focus() == id, targets_);
		view.caret = result.caret;
		animating |= result.animating;
		transitioning |= result.transitioning;
		// Timed leaves (spinners, progress sweeps, elapsed clocks) redraw on their wake-ups, so
		// they are animation areas too: the next frame repaints just them.
		if (result.animating || result.nextWake)
			animated.push_back(gfx::intersect(rect, clips.current()));
		wakeAt(result.nextWake);
		if (result.sizeChanged && YGNodeHasMeasureFunc(yoga)) {
			YGNodeMarkDirty(yoga);
			wakeAt(SDL_GetTicks() + 1);
			relayout = true; // Neighbours move: only a full paint shows that.
		}
	}
	const float bodyTop =
	    view.leaf.toolAgent ? view.leaf.toolAgent->ownHeight : YGNodeLayoutGetPadding(yoga, YGEdgeTop);
	const float bodyBottom = rect.h - YGNodeLayoutGetPadding(yoga, YGEdgeBottom);
	const bool previewClip = collapsed(id) && hasPreview(node) && YGNodeStyleGetOverflow(yoga) == YGOverflowHidden;
	const bool overlayClip = node.kind == tsp::Kind::Overlay && view.scrollMax > 0;
	const bool tail = previewClip && node.prop("preview").is_object() && node.prop("preview").contains("tail");
	float childScroll = node.id == "main" && node.parent == doc_.root() ? 0 : view.scroll;
	if (tail) {
		float bottom = bodyTop;
		for (const auto child : node.children)
			bottom = std::max(bottom,
			                  YGNodeLayoutGetTop(at(child).yoga.get()) + YGNodeLayoutGetHeight(at(child).yoga.get()));
		childScroll = std::max(0.0f, bottom - bodyBottom);
	}
	if (previewClip || overlayClip)
		clips.push({rect.x, rect.y + bodyTop, rect.w, std::max(0.0f, bodyBottom - bodyTop)});
	for (const auto child : node.children)
		animating |= paintNode(child, rect.x, rect.y - childScroll, renderer, clips);
	if (overlayClip && !previewClip)
		clips.pop();
	if (previewClip) {
		const auto fill = (node.kind == tsp::Kind::Card || node.kind == tsp::Kind::Tool) && !view.frameless
		                    ? kinds::cardFill(node)
		                    : text::palette::canvas;
		auto clear = fill;
		clear.a = 0;
		// A gradient, then a solid half line at the cut, so a block clipped to a sliver there
		// (a code block's top edge) doesn't show as a stray band.
		const float fade = std::min(pitch * 1.5f, std::max(0.0f, bodyBottom - bodyTop)), solid = fade / 3;
		const float edge = tail ? bodyTop : bodyBottom - solid;
		gfx::fill(renderer, {rect.x, rect.y + edge, rect.w, solid}, fill);
		gfx::gradientRounded(renderer,
		                     {rect.x, rect.y + (tail ? bodyTop + solid : bodyBottom - fade), rect.w, fade - solid}, 0,
		                     tail ? std::array<SDL_Color, 4>{fill, fill, clear, clear}
		                          : std::array<SDL_Color, 4>{clear, clear, fill, fill});
		clips.pop();
	}
	if (view.headerNode && node.flag("collapsible")) {
		float middle = YGNodeLayoutGetHeight(at(view.headerNode).yoga.get()) * 0.5f, left = 0;
		for (auto heading = view.headerNode; heading != id; heading = doc_.at(heading).parent) {
			middle += YGNodeLayoutGetTop(at(heading).yoga.get());
			left += YGNodeLayoutGetLeft(at(heading).yoga.get());
		}
		const float pad = node.kind == tsp::Kind::Card && !view.frameless ? em * gfx::space::lg : 0;
		disclosure(
		    renderer,
		    {node.kind == tsp::Kind::Section ? rect.x + em * disclosureEms * 0.4f : rect.x + rect.w - pad - em * 0.5f,
		     rect.y + middle},
		    em, collapsed(id));
		// A folded thought shows the start of what it says, so a run of them isn't a column of
		// identical "Thought" lines.
		if (node.kind == tsp::Kind::Section && collapsed(id) && node.str("role") == "omp.thinking") {
			if (view.header.empty())
				view.header = text::toRuns(thoughtExcerpt(doc_, node), {.color = text::palette::dim});
			const float x = rect.x + left + at(view.headerNode).leaf.layout.width + em * 0.6f;
			const float width = rect.x + rect.w - x;
			if (!view.header.empty() && width > em * 4) {
				if (view.headerWidth != width) {
					view.headerLayout =
					    engine_.get().layout(view.header, {.width = width, .wrap = text::Wrap::None, .maxLines = 1});
					view.headerWidth = width;
				}
				engine_.get().draw(view.header, view.headerLayout, x,
				                   rect.y + middle - view.headerLayout.height * 0.5f);
			}
		}
	}
	if (node.str("mark") == "drop")
		kinds::paintDrop(renderer, rect, scale);
	if ((scrollBox || scrollLeaf) && view.scrollMax > 0) {
		const float content = rect.h + view.scrollMax, thumb = std::max(em * 1.5f, rect.h * rect.h / content);
		gfx::rounded(
		    renderer,
		    {rect.x + rect.w - 4 * scale, rect.y + (rect.h - thumb) * view.scroll / view.scrollMax, 2 * scale, thumb},
		    scale, text::palette::dim);
	}
	if (clip)
		clips.pop();
	view.paintDirty = false;
	return animating;
}
bool Tree::paint(std::string_view region, float x, float y, SDL_Renderer& renderer, gfx::ClipStack& clips) {
	const auto id = doc_.find(region);
	if (!id)
		return false;
	const auto yoga = at(*id).yoga.get();
	return paintNode(*id, x - YGNodeLayoutGetLeft(yoga), y - YGNodeLayoutGetTop(yoga), renderer, clips);
}
std::optional<SDL_FRect> Tree::focusedCaret() const {
	const auto focus = doc_.focus();
	if (!focus || !at(*focus).caret)
		return std::nullopt;
	const auto rect = gfx::intersect(*at(*focus).caret, at(*focus).hitRect);
	return rect.w > 0 && rect.h > 0 ? std::optional(rect) : std::nullopt;
}
bool Tree::appPage() const {
	// The rewind page replays the transcript's own blocks, so it reads like the transcript.
	return surface_.mode == "screen" && surface_.role != "omp.rewind";
}
float Tree::asideWidth(float viewportWidth, float preferred) const {
	const auto layer = doc_.find("layer");
	if (!layer)
		return 0;
	for (const auto child : doc_.at(*layer).children) {
		const auto& node = doc_.at(child);
		if (node.kind != tsp::Kind::Prefs || node.flag("hidden"))
			continue;
		const float scale = fonts_.size() / 15;
		if (viewportWidth < 700 * scale)
			return viewportWidth;
		// At least 300 (the sheet's controls stay usable), leaving the transcript at least 360.
		const float least = 300 * scale, most = std::max(least, viewportWidth - 360 * scale);
		return std::clamp(preferred * scale, least, most);
	}
	return 0;
}
float Tree::presentedAsideWidth(float width) const {
	const auto layer = doc_.find("layer");
	if (width > 0 && layer)
		for (const auto child : doc_.at(*layer).children)
			if (doc_.at(child).kind == tsp::Kind::Prefs && !doc_.at(child).flag("hidden")) {
				const auto& view = at(child);
				return width
				     * (view.revealPaint == paintSerial_ ? view.revealValue : view.reveal.value(SDL_GetTicks()));
			}
	return width;
}
bool Tree::prepareAside(float width, bool immediate) {
	const auto layer = doc_.find("layer");
	if (!layer || width <= 0)
		return false;
	for (const auto child : doc_.at(*layer).children)
		if (doc_.at(child).kind == tsp::Kind::Prefs && !doc_.at(child).flag("hidden")) {
			auto& view = at(child);
			YGNodeStyleSetWidth(view.yoga.get(), width);
			if (immediate) {
				view.reveal.snap(1);
				view.revealValue = 1;
				view.revealPaint = paintSerial_;
			} else
				reveal(view, 180);
			return view.revealValue != 1;
		}
	return false;
}
void Tree::scrollCommands(float viewportHeight, float contentHeight) {
	const auto main = doc_.find("main");
	if (!main) {
		doc_.reveals.clear();
		doc_.scrolls.clear();
		return;
	}
	auto& view = at(*main);
	const float max = std::max(0.0f, contentHeight - viewportHeight);
	for (const auto& [id, where] : doc_.reveals) {
		if (where == "end") {
			view.snapScroll(max);
			view.stick = true;
			continue;
		}
		const auto target = doc_.find(id);
		if (!target)
			continue;
		// Reveal scrolls the nearest scrolling ancestor; layer rows must never move the transcript.
		float y = 0;
		auto scroller = *target;
		for (; scroller && (scroller == *target || (scroller != *main && at(scroller).scrollMax <= 0));
		     scroller = doc_.at(scroller).parent)
			y += YGNodeLayoutGetTop(at(scroller).yoga.get());
		if (!scroller)
			continue;
		auto& owner = scroller == *main ? view : at(scroller);
		owner.sampleScroll(SDL_GetTicks());
		const float ownerHeight = scroller == *main ? viewportHeight : owner.rect.h;
		if (scroller == *main)
			y += contentHeight - height("main");
		const float h = YGNodeLayoutGetHeight(at(*target).yoga.get());
		if (where == "center")
			owner.scroll = y - (ownerHeight - h) * 0.5f;
		else if (where == "start")
			owner.scroll = y;
		else if (y < owner.scroll)
			owner.scroll = y;
		else if (y + h > owner.scroll + ownerHeight)
			owner.scroll = y + h - ownerHeight;
		if (scroller == *main)
			view.stick = false;
		owner.snapScroll(std::clamp(owner.scroll, 0.0f, scroller == *main ? max : owner.scrollMax));
	}
	for (const auto& [id, by] : doc_.scrolls) {
		if (by == "end") {
			view.snapScroll(max);
			view.stick = true;
		} else {
			view.sampleScroll(SDL_GetTicks());
			if (by == "start")
				view.scroll = 0;
			else
				view.scroll += (by == "line-up"     ? -fonts_.lineHeight()
				                : by == "line-down" ? fonts_.lineHeight()
				                : by == "page-up"   ? -viewportHeight + fonts_.lineHeight()
				                                    : viewportHeight - fonts_.lineHeight());
			view.stick = false;
		}
		view.snapScroll(std::clamp(view.scroll, 0.0f, max));
	}
	doc_.reveals.clear();
	doc_.scrolls.clear();
}

uint64_t Tree::signature(tsp::NodeId id) const {
	// Changes anywhere below bump some node's revision; local folds change the laid-out size.
	uint64_t hash = 1469598103934665603ull;
	const auto mix = [&](uint64_t value) {
		hash = (hash ^ value) * 1099511628211ull;
	};
	const auto yoga = at(id).yoga.get();
	mix(std::bit_cast<uint32_t>(YGNodeLayoutGetWidth(yoga)));
	mix(std::bit_cast<uint32_t>(YGNodeLayoutGetHeight(yoga)));
	std::vector<tsp::NodeId> stack{id};
	while (!stack.empty()) {
		const auto& node = doc_.at(stack.back());
		stack.pop_back();
		mix(node.rev);
		mix(node.children.size());
		stack.insert(stack.end(), node.children.begin(), node.children.end());
	}
	return hash;
}
void Tree::collectOverview(tsp::NodeId id, float x, float y, SDL_FRect clip, std::vector<kinds::OverviewBar>& out) {
	const auto& node = doc_.at(id);
	auto& view = at(id);
	const auto yoga = view.yoga.get();
	if (YGNodeStyleGetDisplay(yoga) == YGDisplayNone)
		return;
	const SDL_FRect rect{x, y, YGNodeLayoutGetWidth(yoga), YGNodeLayoutGetHeight(yoga)};
	const auto add = [&](kinds::OverviewBar bar) {
		bar.rect = gfx::intersect(bar.rect, clip);
		if (bar.rect.w > 0 && bar.rect.h > 0)
			out.push_back(bar);
	};
	const bool framed =
	    !view.frameless
	    && (node.kind == tsp::Kind::Card
	        || ((node.kind == tsp::Kind::Tool || node.kind == tsp::Kind::Agent) && node.str("frame") != "inline"));
	if (framed)
		add({rect, kinds::cardFill(node)});
	if (YGNodeHasMeasureFunc(yoga) || node.kind == tsp::Kind::Tool || node.kind == tsp::Kind::Agent) {
		const size_t first = out.size();
		kinds::overview(node, view.leaf, fonts_, {rect.w, rect.h}, out);
		std::vector<kinds::OverviewBar> leaf(out.begin() + static_cast<ptrdiff_t>(first), out.end());
		out.resize(first);
		for (auto bar : leaf) {
			bar.rect.x += x;
			bar.rect.y += y;
			add(bar);
		}
	}
	if (YGNodeStyleGetOverflow(yoga) != YGOverflowVisible)
		clip = gfx::intersect(clip, rect);
	const float scroll = YGNodeStyleGetOverflow(yoga) == YGOverflowScroll && node.id != "main" ? view.scroll : 0;
	for (const auto child : node.children) {
		const auto childYoga = at(child).yoga.get();
		collectOverview(child, x + YGNodeLayoutGetLeft(childYoga), y + YGNodeLayoutGetTop(childYoga) - scroll, clip,
		                out);
	}
}
const std::vector<kinds::OverviewBar>& Tree::overview(tsp::NodeId child) {
	auto& entry = overview_[child];
	const auto current = signature(child);
	if (entry.signature != current || entry.bars.empty()) {
		entry.signature = current;
		entry.bars.clear();
		collectOverview(child, 0, 0, {-1e7f, -1e7f, 2e7f, 2e7f}, entry.bars);
	}
	return entry.bars;
}

Views::Views(tsp::Session& session, SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts)
    : session_(session),
      renderer_(renderer),
      engine_(engine),
      fonts_(fonts),
      clips_(std::in_place, renderer),
      pointer_(std::make_unique<PointerState>()) {
	session_.surfaceOpened = [this](auto& surface) {
		trees_.emplace(surface.id, std::make_unique<Tree>(
		                               surface, engine_.get(), fonts_,
		                               [this](int delta) {
			                               runningCount_ += delta;
			                               if (onRunningChanged)
				                               onRunningChanged(runningCount_);
		                               },
		                               [this](std::string_view blob) { return image(blob); }));
	};
	session_.blobAdded = [this](const std::string& blob) {
		images_.erase(blob); // Re-sent bytes for the same address replace a failed decode.
		for (auto& [id, tree] : trees_)
			tree->blobArrived(blob);
	};
	session_.surfaceClosing = [this](auto& surface) {
		dismissHover();
		active_.reset();
		scrollOwner_.reset();
		caret_.reset();
		painted_.clear();
		kept_.clear();
		trees_.erase(surface.id);
		if (trees_.empty()) {
			transitioning_ = false;
			nextWake_ = relayoutAt_ = 0;
		}
	};
	// Keep/adopt changes presentation order, never the attached document/view pair.
	session_.surfaceChanged = [this](auto&) {
		dismissHover();
		caret_.reset();
	};
	for (auto& surface : session_.surfaces())
		session_.surfaceOpened(*surface);
}
Views::~Views() {
	dismissHover();
	session_.teardownAll();
	session_.surfaceOpened = {};
	session_.surfaceClosing = {};
	session_.surfaceChanged = {};
	session_.blobAdded = {};
}
std::shared_ptr<const gfx::Image> Views::image(std::string_view blob) {
	const std::string key(blob);
	if (const auto found = images_.find(key); found != images_.end())
		return found->second;
	const auto bytes = session_.blob(blob);
	if (!bytes)
		return nullptr; // Not arrived yet: blobAdded asks again.
	auto decoded = gfx::Image::decode(gfx::decodeBase64(*bytes));
	auto picture = decoded ? std::make_shared<const gfx::Image>(std::move(*decoded)) : nullptr;
	images_.emplace(key, picture);
	return picture;
}
void Views::rebind(SDL_Renderer& renderer, text::Engine& engine) {
	if (&renderer_.get() == &renderer && &engine_.get() == &engine)
		return;
	dismissHover();
	for (auto& [id, tree] : trees_)
		tree->rebind(engine);
	renderer_ = renderer;
	engine_ = engine;
	clips_.emplace(renderer);
	caret_.reset();
}
void Views::metricsChanged() {
	dismissHover();
	selection_.reset(); // Its pixel endpoints no longer address the same text.
	for (auto& [id, tree] : trees_)
		tree->metricsChanged();
}
void Views::themeChanged() {
	// Leaf caches hold runs coloured at sync time; drop them so everything re-syncs in the new
	// palette, keeping only what the user expanded.
	for (auto& [id, tree] : trees_)
		tree->forgetLeaves();
	images_.clear(); // Pictures are cheap to decode again; SVG theme colours come from the palette.
	metricsChanged();
}
float Views::contentWidth(float viewportWidth) const {
	if (const auto screen = session_.screen()) {
		const auto& tree = *trees_.at(screen->get().id);
		return std::max(1.0f, viewportWidth - tree.asideWidth(viewportWidth, asidePreferred_));
	}
	for (auto it = session_.surfaces().rbegin(); it != session_.surfaces().rend(); ++it)
		if ((*it)->open && (*it)->mode != "screen")
			return std::max(1.0f, viewportWidth - trees_.at((*it)->id)->asideWidth(viewportWidth, asidePreferred_));
	return viewportWidth;
}
void Views::setWidthTransition(bool active, float finalViewportWidth) {
	if (widthTransition_ && !active) {
		forceWidthLayout_ = true;
		relayoutAt_ = 0;
		pendingWidth_ = -1;
	}
	widthTransition_ = active;
	widthTransitionViewport_ = finalViewportWidth;
}
bool Views::paint(const SDL_FRect& viewport, Paint mode) {
	const bool partial = mode != Paint::Full && !transitioning(), more = partial && mode == Paint::PartialMore;
	if (!more && !gather_)
		nextWake_ = 0;
	if (!more && !gather_)
		transitioning_ = false;
	lastViewport_ = viewport;
	painted_.clear();
	active_.reset();
	scrollOwner_.reset();
	caret_.reset();
	const auto screen = session_.screen();
	// While the transcript scrolls, the minimap has a lane at the right edge (its width plus a
	// gap to the window edge) and the column and dock centre in the area left of it, so their
	// margins match on both sides; otherwise they centre in the whole width.
	const float em = fonts_.size();
	kept_.clear();
	for (const auto& surface : session_.surfaces()) {
		if (screen) {
			if (surface->id == screen->get().id)
				active_ = tree(*surface);
		} else if (surface->mode != "screen") {
			if (surface->open)
				active_ = tree(*surface);
			else
				kept_.push_back(tree(*surface));
		}
	}
	// App pages (/git) span the content edge to edge; the transcript and other screens (rewind)
	// get the reading column's margins and the minimap.
	const bool page = active_ && active_->get().appPage();
	const float pad = page ? 0 : em * gfx::space::lg;
	if (!active_ && kept_.empty())
		return false;
	scrollOwner_ = active_;
	if (!scrollOwner_)
		scrollOwner_ = kept_.back();
	size_t dirty = 0, measured = 0;
	nodeCount_ = 0;
	bool relaid = false;
	for (auto& [id, tree] : trees_) {
		// Copy gathers are unpresented: keep hits, panel visibility and pending animation state.
		tree->beginPaint(partial || gather_, more || gather_, paintedOnce_);
		nodeCount_ += tree->document().size();
	}
	const float finalViewportWidth = widthTransition_ ? widthTransitionViewport_ : viewport.w;
	const float finalAsideWidth = active_ ? active_->get().asideWidth(finalViewportWidth, asidePreferred_) : 0;
	// A viewport-clamped aside follows the same narrower-endpoint rule as the column.
	asideLayoutWidth_ = widthTransition_ && asideLayoutWidth_ > 0 && finalAsideWidth > 0
	                      ? std::min(asideLayoutWidth_, finalAsideWidth)
	                      : finalAsideWidth;
	const bool hasAside = asideLayoutWidth_ > 0;
	const bool asideMoving = hasAside && active_->get().prepareAside(asideLayoutWidth_, asideDrag_.has_value());
	if (!asideMoving && (asideTransition_ || hasAside != asideVisible_))
		forceWidthLayout_ = true;
	asideTransition_ = asideMoving;
	asideVisible_ = hasAside;
	const float reservedAside = active_ ? active_->get().presentedAsideWidth(asideLayoutWidth_) : 0;
	const float presentedWidth = std::max(1.0f, viewport.w - reservedAside);
	const SDL_FRect content{viewport.x, viewport.y, presentedWidth, viewport.h};
	const float grip = fonts_.size() * 0.3f;
	asideGrip_ = content.w < viewport.w && content.w > 1
	               ? SDL_FRect{content.x + content.w - grip, viewport.y, grip * 2, viewport.h}
	               : SDL_FRect{};
	float area = content.w;
	const bool freezeWidth = widthTransition_ || asideMoving;
	const float finalContentWidth = std::max(1.0f, finalViewportWidth - finalAsideWidth);
	if (freezeWidth)
		relayoutAt_ = 0;
	float dockHeight = 0, maxScroll = 0;
	auto layOut = [&](bool lane) {
		area = std::max(1.0f, content.w - (lane ? em * (minimapEms + 1) : 0));
		float innerWidth = std::max(1.0f, area - 2 * pad);
		// When the last re-wrap was slow, a new width waits until it has held for a moment (the
		// window being dragged), painting the column at its laid-out width meanwhile.
		constexpr Uint64 settle = 150;
		const Uint64 now = SDL_GetTicks();
		if (freezeWidth) {
			// Always slide at the narrower endpoint: it fits throughout the transition. A
			// narrowing re-wrap happens once now; widening keeps its old width until the end.
			const float finalArea = std::max(1.0f, finalContentWidth - (lane ? em * (minimapEms + 1) : 0));
			const float finalWidth = std::max(1.0f, finalArea - 2 * pad);
			innerWidth = laidWidth_ > 0 ? std::min(laidWidth_, finalWidth) : finalWidth;
		} else if (laidWidth_ > 0 && innerWidth != laidWidth_ && slowRelayout_ && !forceWidthLayout_) {
			if (innerWidth != pendingWidth_) {
				pendingWidth_ = innerWidth;
				pendingSince_ = now;
			}
			if (now < pendingSince_ + settle) {
				innerWidth = laidWidth_;
				relayoutAt_ = pendingSince_ + settle;
			}
		}
		const bool rewrap = innerWidth != laidWidth_;
		const auto started = SDL_GetPerformanceCounter();
		auto lay = [&](Tree& tree) {
			tree.layout(innerWidth, viewport.h);
			if (tree.laidOut) {
				relaid = true;
				dirty += tree.lastDirty;
				measured += tree.lastMeasured;
			}
		};
		for (auto& tree : kept_)
			lay(tree.get());
		if (active_)
			lay(active_->get());
		if (rewrap) {
			laidWidth_ = innerWidth;
			pendingWidth_ = -1;
			relayoutAt_ = 0;
			slowRelayout_ = (SDL_GetPerformanceCounter() - started) * 1000 > SDL_GetPerformanceFrequency() * 40;
		}
		dockHeight = active_ ? active_->get().height("dock") : 0;
		const float dockSpace = dockHeight > 0 ? dockHeight + em * 2 : 0;
		mainClip_ = {content.x, content.y, content.w, std::max(0.0f, content.h - dockSpace)};
		contentHeight_ = pad * 1.2f;
		for (auto& tree : kept_)
			contentHeight_ += tree.get().height("main") + em * gfx::space::lg;
		if (active_)
			contentHeight_ += active_->get().height("main");
		maxScroll = std::max(0.0f, contentHeight_ - mainClip_.h);
	};
	// Last frame's choice first. Flipping it settles at once: the lane only narrows the column
	// (taller, still scrolling) and dropping it only widens it (shorter, still fitting).
	layOut(minimapLane_ && !page);
	if (!page && (maxScroll > 0) != minimapLane_) {
		minimapLane_ = maxScroll > 0;
		layOut(minimapLane_);
	}
	if (!freezeWidth)
		forceWidthLayout_ = false;
	if (relaid) {
		lastDirty_ = dirty;
		lastMeasured_ = measured;
	}
	float scroll = 0;
	if (scrollOwner_) {
		auto& tree = scrollOwner_->get();
		if (const auto main = tree.document().find("main")) {
			auto& view = tree.at(*main);
			const Uint64 now = SDL_GetTicks();
			const bool changedBounds = view.scrollMax != maxScroll;
			view.scrollMax = maxScroll;
			view.sampleScroll(now);
			tree.scrollCommands(mainClip_.h, contentHeight_);
			if (view.stick && (changedBounds || !view.scrollTween.active(now)))
				view.snapScroll(maxScroll);
			tree.transitioning |= view.scroll != view.scrollTween.target();
			scroll = view.scroll;
		}
	}
	auto& clips = *clips_;
	// A gather pass paints the whole transcript unclipped, so a copy includes text scrolled out of view.
	const SDL_FRect everywhere{content.x, -1e7f, content.w, 2e7f};
	clips.push(gather_ ? everywhere : viewport);
	clips.push(gather_ ? everywhere : mainClip_);
	float y = viewport.y + pad * 0.6f - scroll;
	bool animating = false;
	mainScroll_ = scroll;
	if (selection_) {
		// Endpoints live in transcript space so the selection scrolls with its text.
		SDL_FPoint a{selection_->first.x, selection_->first.y - scroll},
		    b{selection_->second.x, selection_->second.y - scroll};
		const bool sameLine = std::abs(a.y - b.y) < fonts_.lineHeight() * 0.5f;
		if (sameLine ? b.x < a.x : b.y < a.y)
			std::swap(a, b);
		engine_.get().setSelection(text::Selection{a, b, selectWord_});
	}
	auto columnX = [&](Tree& tree, std::string_view region) {
		const auto id = tree.document().find(region);
		const float width = id ? YGNodeLayoutGetWidth(tree.at(*id).yoga.get()) : 0;
		return content.x + (area - width) * 0.5f;
	};
	for (auto& tree : kept_) {
		// Main's scroll belongs to the shared transcript, not each kept surface.
		animating |= tree.get().paint("main", columnX(tree, "main"), y, renderer_.get(), clips);
		painted_.push_back(tree);
		y += tree.get().height("main") + em * gfx::space::sm;
		gfx::line(renderer_.get(), {content.x + pad, y}, {content.x + content.w - pad, y}, text::palette::border);
		y += em * gfx::space::sm;
	}
	if (active_) {
		auto& tree = active_->get();
		animating |= tree.paint("main", columnX(tree, "main"), y, renderer_.get(), clips);
		painted_.push_back(tree);
	}
	engine_.get().setSelection(std::nullopt);
	clips.pop();
	if (gather_) {
		clips.pop();
		return false;
	}
	if (!partial) {
		// Both run the full height of the content: the minimap in its lane, a thin scrollbar at
		// the very edge. A partial repaint keeps the last ones.
		minimap_ = {};
		const auto main = scrollOwner_ ? scrollOwner_->get().document().find("main") : std::nullopt;
		const float mainWidth = main ? YGNodeLayoutGetWidth(scrollOwner_->get().at(*main).yoga.get()) : 0;
		if (maxScroll > 0 && mainClip_.h > 0) {
			if (!page && mainWidth > 0) {
				minimap_ = {content.x + content.w - em * (minimapEms + 1), content.y, em * minimapEms, content.h};
				paintMinimap(scroll, maxScroll, pad, mainWidth);
			}
			const float thumb = std::max(em * 1.5f, content.h * mainClip_.h / contentHeight_);
			gfx::rounded(renderer_.get(),
			             {content.x + content.w - 4, content.y + (content.h - thumb) * scroll / maxScroll, 2, thumb}, 1,
			             text::palette::dim);
		}
	}
	if (active_) {
		auto& tree = active_->get();
		const float dockY = viewport.y + viewport.h - dockHeight - em * 1.2f;
		clips.push(content);
		animating |= tree.paint("dock", columnX(tree, "dock"), dockY, renderer_.get(), clips);
		clips.pop();
		animating |= tree.paintLayer(viewport, dockY, asideLayoutWidth_, renderer_.get(), clips);
		if (asideGrip_.w > 0 && (asideDrag_ || contains(asideGrip_, pointer_->position.x, pointer_->position.y)))
			gfx::fill(renderer_.get(), {content.x + content.w - 1, viewport.y, 2, viewport.h}, text::palette::accent);
		caret_ = tree.focusedCaret();
		if (caret_) {
			caret_->x = std::clamp(caret_->x, viewport.x, viewport.x + viewport.w - caret_->w);
			caret_->y = std::clamp(caret_->y, viewport.y, viewport.y + viewport.h - caret_->h);
		}
	}
	bool elsewhere = false;
	if (pointer_->position.x >= 0)
		elsewhere |= motion(pointer_->position.x, pointer_->position.y, true);
	elsewhere |= paintHover(viewport);
	animatedElsewhere_ = elsewhere || (more && animatedElsewhere_);
	animating |= elsewhere;
	paintMenu();
	auto schedule = [&](Uint64 tick) {
		if (tick && (!nextWake_ || tick < nextWake_))
			nextWake_ = tick;
	};
	for (auto& painted : painted_)
		schedule(painted.get().nextWake);
	if (pointer_->card)
		schedule(pointer_->card->nextWake);
	transitioning_ |= widthTransition_ || asideMoving;
	for (const auto& painted : painted_)
		transitioning_ |= painted.get().transitioning;
	if (pointer_->card)
		transitioning_ |= pointer_->card->transitioning;
	paintedOnce_ = true;
	clips.pop();
	// A full paint has drawn every visible picture, so the rest were scrolled away (or belong to
	// another tab sharing this window's engine).
	if (!partial)
		engine_.get().trimPictures();
	return animating || transitioning();
}
std::optional<std::vector<SDL_FRect>> Views::animatedRegions() const {
	if (animatedElsewhere_ || transitioning())
		return std::nullopt;
	std::vector<SDL_FRect> regions;
	for (const auto& painted : painted_) {
		if (painted.get().relayout)
			return std::nullopt;
		regions.insert(regions.end(), painted.get().animated.begin(), painted.get().animated.end());
	}
	return regions;
}
void Views::paintMinimap(float scroll, float maxScroll, float pad, float mainWidth) {
	const float em = fonts_.size(), unit = em / 15, s = minimap_.w / mainWidth;
	const float total = contentHeight_ * s;
	minimapScale_ = s;
	// Longer than its strip, the overview scrolls with the transcript, ending together.
	minimapScroll_ = total > minimap_.h && maxScroll > 0 ? scroll / maxScroll * (total - minimap_.h) : 0;
	const float from = minimapScroll_ / s, to = (minimapScroll_ + minimap_.h) / s;
	minimapVertices_.clear();
	minimapIndices_.clear();
	const auto quad = [&](SDL_FRect rect, SDL_Color color) {
		const auto base = static_cast<int>(minimapVertices_.size());
		const SDL_FColor fill{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
		minimapVertices_.push_back({{rect.x, rect.y}, fill, {}});
		minimapVertices_.push_back({{rect.x + rect.w, rect.y}, fill, {}});
		minimapVertices_.push_back({{rect.x + rect.w, rect.y + rect.h}, fill, {}});
		minimapVertices_.push_back({{rect.x, rect.y + rect.h}, fill, {}});
		for (const int index : {0, 1, 2, 0, 2, 3})
			minimapIndices_.push_back(base + index);
	};
	// Transcript space as painted: kept surfaces first, each followed by a divider, then the live one.
	float top = pad * 0.6f;
	const auto add = [&](Tree& tree) {
		const auto main = tree.document().find("main");
		if (!main)
			return;
		for (const auto child : tree.document().at(*main).children) {
			const auto yoga = tree.at(child).yoga.get();
			if (YGNodeStyleGetDisplay(yoga) == YGDisplayNone)
				continue;
			const float childTop = top + YGNodeLayoutGetTop(yoga), left = YGNodeLayoutGetLeft(yoga);
			if (childTop + YGNodeLayoutGetHeight(yoga) < from || childTop > to)
				continue;
			for (const auto& bar : tree.overview(child)) {
				const float y = minimap_.y + (childTop + bar.rect.y) * s - minimapScroll_;
				if (y > minimap_.y + minimap_.h || y + bar.rect.h * s < minimap_.y)
					continue;
				auto color = bar.color;
				color.a = static_cast<Uint8>(color.a * 0.8f);
				quad({minimap_.x + (left + bar.rect.x) * s, y, std::max(unit * 0.5f, bar.rect.w * s),
				      std::max(unit * 0.5f, bar.rect.h * s)},
				     color);
			}
		}
	};
	for (auto& tree : kept_) {
		add(tree.get());
		top += tree.get().height("main") + em * gfx::space::sm * 2;
	}
	if (active_)
		add(active_->get());
	auto& clips = *clips_;
	clips.push(minimap_);
	if (!minimapIndices_.empty())
		SDL_RenderGeometry(&renderer_.get(), nullptr, minimapVertices_.data(),
		                   static_cast<int>(minimapVertices_.size()), minimapIndices_.data(),
		                   static_cast<int>(minimapIndices_.size()));
	// The part of the transcript in view.
	const SDL_FRect slider{minimap_.x, minimap_.y + scroll * s - minimapScroll_, minimap_.w, mainClip_.h * s};
	gfx::fill(renderer_.get(), slider, text::withAlpha(text::palette::fg, minimapDrag_ ? 56 : 36));
	clips.pop();
}
void Views::scrollMainTo(float value) {
	if (!scrollOwner_)
		return;
	auto& tree = scrollOwner_->get();
	const auto main = tree.document().find("main");
	if (!main)
		return;
	auto& view = tree.at(*main);
	const float max = std::max(0.0f, contentHeight_ - mainClip_.h);
	view.snapScroll(std::clamp(value, 0.0f, max));
	view.stick = view.scroll >= max - 2;
	mainScroll_ = view.scroll;
}
bool Views::wheel(float x, float y, float deltaPixels) {
	dismissHover();
	pointer_->suppressed = true;
	for (auto it = painted_.rbegin(); it != painted_.rend(); ++it) {
		auto& tree = it->get();
		if (tree.scrollAt(x, y, deltaPixels)) {
			transitioning_ |= tree.transitioning;
			return true;
		}
		if (const auto target = tree.hit(x, y); target && target->get().kind == TargetKind::Block)
			return true;
	}
	if (!scrollOwner_ || !(contains(mainClip_, x, y) || contains(minimap_, x, y)))
		return false;
	auto& tree = scrollOwner_->get();
	const auto main = tree.document().find("main");
	if (!main)
		return false;
	auto& view = tree.at(*main);
	const float max = std::max(0.0f, contentHeight_ - mainClip_.h);
	view.scrollMax = max;
	view.wheelScroll(deltaPixels, SDL_GetTicks());
	transitioning_ |= view.scroll != view.scrollTween.target();
	return true;
}
std::optional<SDL_FRect> Views::nodeRect(std::string_view surface, std::string_view id) const {
	const auto found = trees_.find(std::string(surface));
	if (found == trees_.end())
		return std::nullopt;
	const auto node = found->second->document().find(id);
	return node ? std::optional(found->second->at(*node).rect) : std::nullopt;
}
std::optional<SDL_FRect> Views::caretRect(std::string_view surface, std::string_view id) const {
	const auto found = trees_.find(std::string(surface));
	if (found == trees_.end())
		return std::nullopt;
	const auto node = found->second->document().find(id);
	if (!node || !found->second->at(*node).caret)
		return std::nullopt;
	const auto& view = found->second->at(*node);
	const auto rect = gfx::intersect(*view.caret, view.hitRect);
	return rect.w > 0 && rect.h > 0 ? std::optional(rect) : std::nullopt;
}
std::string Views::dirtySummary() const {
	return std::format("Yoga last layout: {} dirty / {} nodes · {} leaf measures (clean branches cached)", lastDirty_,
	                   nodeCount_, lastMeasured_);
}

} // namespace view
