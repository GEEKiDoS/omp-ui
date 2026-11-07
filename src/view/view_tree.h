#pragma once

#include "core/tsp.h"
#include "gfx/draw.h"
#include "gfx/tween.h"
#include "kinds/leaf.h"
#include "search.h"
#include <yoga/Yoga.h>
#include <functional>
#include <memory>
#include <unordered_map>

namespace view {

struct YogaDeleter {
	void operator()(YGNode* value) const { YGNodeFree(value); }
};
class Tree;
struct PointerState;
struct ImageViewer;
struct NodeView {
	NodeView(Tree& tree, tsp::NodeId node) : tree(tree), id(node), yoga(YGNodeNew()) {}
	Tree& tree;
	tsp::NodeId id;
	std::unique_ptr<YGNode, YogaDeleter> yoga;
	kinds::LeafCache leaf;
	text::Runs header, mark;
	text::Layout headerLayout, markLayout;
	float headerWidth = -1, headerHeight = 0;
	tsp::NodeId headerNode = 0;
	std::optional<bool> collapsed;
	float scroll = 0;
	float scrollMax = 0;
	gfx::Tween scrollTween, reveal;
	Uint64 revealPaint = 0;
	float revealValue = 1;
	void snapScroll(float value);
	void sampleScroll(Uint64 now);
	void wheelScroll(float delta, Uint64 now);
	// Collapsed with a preview: the most of its content it can show (pixels, 0: no limit).
	float previewBudget = 0;
	std::string selected;
	Uint64 arrived = SDL_GetTicks();
	bool stick = true, paintDirty = true, priorityHidden = false, frameless = false, listEmpty = false;
	bool running = false;
	SDL_FRect rect{}, hitRect{};
	std::optional<SDL_FRect> caret;
};
// Decoded `image` blobs by content address; null while a blob is missing or undecodable.
using ImageLookup = std::function<std::shared_ptr<const gfx::Image>(std::string_view blob)>;

class Tree {
public:
	// The optional callback receives running-node count deltas, including construction and teardown.
	Tree(tsp::Surface& surface, text::Engine& engine, const text::Fonts& fonts,
	     std::function<void(int)> runningChanged = {}, ImageLookup images = {});
	~Tree();
	Tree(const Tree&) = delete;
	Tree& operator=(const Tree&) = delete;
	// Discard renderer-owned text while preserving node, Yoga and scroll state.
	void rebind(text::Engine& engine);
	void metricsChanged();
	// Drop every leaf's cached runs and pictures (a palette change); metricsChanged() re-syncs.
	void forgetLeaves();
	void layout(float width, float viewportHeight = 0);
	float height(std::string_view region) const;
	// The width of a docked aside (omp's settings sheet) at `preferred` (pixels at scale 1, as the
	// user dragged it), clamped so the transcript keeps room; the whole width when narrow.
	float asideWidth(float viewportWidth, float preferred) const;
	// A screen surface showing a page that fills the window (/git, full-screen pickers and
	// settings, omp's debug and setup pages); not the rewind page, which looks like the transcript.
	bool appPage() const;
	float presentedAsideWidth(float layoutWidth) const;
	// Set its stable endpoint width before transcript layout, then reserve the revealed part.
	bool prepareAside(float layoutWidth, bool immediate);
	bool paint(std::string_view region, float x, float y, SDL_Renderer& renderer, gfx::ClipStack& clips);
	// `partial`: a damage-only redraw (nothing moved), which keeps the last full paint's hit
	// targets and caret; `more`: a further damage region of the same frame, which also keeps
	// the animation areas and wake-up time gathered so far.
	void beginPaint(bool partial = false, bool more = false, bool transitions = true);
	// Rects (output pixels, clipped) of leaves that reported animation in the last paint.
	std::vector<SDL_FRect> animated;
	// A leaf changed size during the last paint, so the layout moved: the next paint must be full.
	bool relayout = false;
	bool transitioning = false;
	std::optional<SDL_FRect> focusedCaret() const;
	std::optional<std::reference_wrapper<const HitTarget>> hit(float x, float y) const { return targets_.hit(x, y); }
	// Whether a press here means a gesture (button, toggle, link, node action, overlay) rather than transcript text.
	bool interactiveAt(float x, float y) const;
	bool click(const HitTarget& target, std::string_view gesture, tsp::Session& session);
	void runAction(tsp::NodeId id, std::string_view name, tsp::Session& session);
	std::vector<std::pair<tsp::NodeId, std::string>> menuActions(float x, float y) const;
	// Editors (composer, inputs): which one is under a point, the UTF-16 offset of a point in it,
	// a terminal-side selection over it, and `edit` events that change it through the program.
	std::optional<tsp::NodeId> editorAt(float x, float y) const;
	size_t editorOffset(tsp::NodeId id, float x, float y);
	void setEditorSelection(tsp::NodeId id, std::optional<std::pair<size_t, size_t>> selection);
	void sendEdit(tsp::NodeId id, size_t from, size_t to, std::string_view text, tsp::Session& session) const;
	bool paintLayer(const SDL_FRect& viewport, float dockY, float asideLayoutWidth, SDL_Renderer& renderer,
	                gfx::ClipStack& clips);
	void setPreedit(std::string_view text, int start, int length);
	bool scrollAt(float x, float y, float delta);
	bool hoverTool(float x, float y);
	void scrollCommands(float viewportHeight, float contentHeight);
	// A blob arrived after the frames that reference it: show it in those `image` nodes.
	void blobArrived(std::string_view blob);
	NodeView& at(tsp::NodeId id) { return *nodes_[id]; }
	const NodeView& at(tsp::NodeId id) const { return *nodes_[id]; }
	tsp::Document& document() { return doc_; }
	const tsp::Surface& surface() const { return surface_; }
	size_t dirtyCount = 0, measuredCount = 0, lastDirty = 0, lastMeasured = 0;
	Uint64 nextWake = 0;
	void wakeAt(Uint64 tick) {
		if (tick && (!nextWake || tick < nextWake))
			nextWake = tick;
	}
	bool laidOut = false;
	// Changes to main's searchable presentation, excluding dock/layer input and timers.
	uint64_t searchRevision = 1;
	// The minimap's marks for one child of main (child-local output pixels): its frames, code
	// backgrounds and text lines. Rebuilt only when something under it changed since last asked.
	const std::vector<kinds::OverviewBar>& overview(tsp::NodeId child);

private:
	static YGSize measure(YGNodeConstRef node, float width, YGMeasureMode widthMode, float height,
	                      YGMeasureMode heightMode);
	static void dirtied(YGNodeConstRef node);
	void added(tsp::NodeId id);
	void removing(tsp::NodeId id);
	void moved(tsp::NodeId id);
	void sync(tsp::NodeId id);
	void syncSubtree(tsp::NodeId id);
	void textChanged(tsp::NodeId id);
	void searchChanged(tsp::NodeId id);
	void display(tsp::NodeId id);
	bool collapsed(tsp::NodeId id) const;
	bool underComposer(tsp::NodeId id) const;
	bool nestedFrame(tsp::NodeId id) const;
	bool descendant(tsp::NodeId node, tsp::NodeId ancestor) const;
	// The preview budget a leaf inherits from its nearest collapsed-preview ancestor (0: none).
	float budgetFor(tsp::NodeId id) const;
	void remeasureLeaves(tsp::NodeId id);
	// The nearest collapsible ancestor (a tool card, thinking section), if any.
	std::optional<tsp::NodeId> foldingAncestor(tsp::NodeId id) const;
	// Whether a code/terminal block shows all its lines: while its card is open, else as the
	// user expanded it ("N more lines" outside any card).
	bool blockExpanded(tsp::NodeId id) const;
	void toolLayout(tsp::NodeId id, float width);
	void statusLayout(tsp::NodeId id, float width);
	void listLayout(tsp::NodeId id, float width);
	bool paintNode(tsp::NodeId id, float parentX, float parentY, SDL_Renderer& renderer, gfx::ClipStack& clips);
	float reveal(NodeView& view, Uint64 duration = 140);
	void toggleNode(tsp::NodeId id, tsp::Session& session);
	tsp::Surface& surface_;
	tsp::Document& doc_;
	std::reference_wrapper<text::Engine> engine_;
	const text::Fonts& fonts_;
	std::function<void(int)> runningChanged_;
	ImageLookup images_;
	std::vector<std::unique_ptr<NodeView>> nodes_;
	HitTargets targets_;
	tsp::NodeId hoveredTool_ = 0;
	// Scrollable boxes and leaves painted in the last full paint (visible rect, node), parents
	// first: the wheel scrolls the innermost one under the pointer.
	std::vector<std::pair<SDL_FRect, tsp::NodeId>> scrollers_;
	bool partial_ = false;
	bool transitionsReady_ = false;
	Uint64 paintSerial_ = 0;
	float width_ = -1, viewportHeight_ = 0;
	struct Overview {
		uint64_t signature = 0;
		std::vector<kinds::OverviewBar> bars;
	};
	std::unordered_map<tsp::NodeId, Overview> overview_;
	uint64_t signature(tsp::NodeId id) const;
	void collectOverview(tsp::NodeId id, float x, float y, SDL_FRect clip, std::vector<kinds::OverviewBar>& out);
};

class Views {
public:
	Views(tsp::Session& session, SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts);
	~Views();
	int runningCount() const { return runningCount_; }
	std::function<void(int)> onRunningChanged; // Receives the total whenever the running-node count changes.
	// Move the existing document views to another window's rendering resources.
	void rebind(SDL_Renderer& renderer, text::Engine& engine);
	void metricsChanged();
	// The palette changed (config reload): rebuild every node's runs in the new colours.
	void themeChanged();
	float contentWidth(float viewportWidth) const;
	// Partial: repaint only within the renderer's current clip (damage of a previous paint's
	// animations), keeping hit targets; only valid when nothing changed since the last full paint.
	// PartialMore: another damage region of the same frame (wake-ups from earlier regions kept).
	enum class Paint { Full, Partial, PartialMore };
	bool paint(const SDL_FRect& viewport, Paint mode = Paint::Full);
	bool transitioning() const { return transitioning_ || widthTransition_; }
	// Fit a narrowing column before the sidebar slides; defer widening until its final frame.
	void setWidthTransition(bool active, float finalViewportWidth);
	// Where the last paint's animations are (output pixels), so the next frame can repaint just
	// those; nullopt when an animation's area is unknown (hover fades) and a full paint is needed.
	std::optional<std::vector<SDL_FRect>> animatedRegions() const;
	bool wheel(float x, float y, float deltaPixels);
	bool handle(const SDL_Event& event, float toOutputX, float toOutputY);
	bool pointerCaptured() const;
	bool imageViewerOpen() const { return imageViewer_ != nullptr; }
	// Draw after all window chrome, not inside the transcript's viewport.
	bool paintImageViewer();
	// Find chrome belongs to this tab, not the program's prompt.
	void openSearch();
	bool searchFocused() const { return search_.open && search_.focused; }
	bool claimSearchKey(SDL_Keycode key, SDL_Keymod mod) const;
	bool searchKey(SDL_Keycode key, SDL_Keymod mod, std::string_view typed = {});
	bool searchText(std::string_view text);
	void searchQuery(std::string_view text);
	void searchOption(std::string_view option, bool enabled);
	void searchNext(bool previous = false);
	json searchState() const;
	// Selection: a terminal-side range in the composer/input or transcript main text.
	bool hasSelection() const { return selection_.has_value() || hasEditorSelection(); }
	bool hasEditorSelection() const;
	void copySelection();
	void cutSelection();
	// Select all of the focused editor's text, or the whole transcript when no editor has text.
	void selectAll();
	// A key typed while an editor range is selected: replaces or deletes it through `edit` events.
	// Returns true when consumed; other keys clear the range and go to the program.
	bool editorKey(SDL_Keycode key, SDL_Keymod mod, std::string_view text);
	// The selected text in reading order, including parts scrolled out of view (repaints once).
	std::string selectionText();
	int wakeDelay() const;
	// What is due now: a full paint (a hover card's delay ran out, or a deferred resize settled)
	// or a timed leaf's frame (its area is in animatedRegions(), so a partial paint suffices).
	enum class Wake { None, Animation, Full };
	Wake tick();
	std::optional<SDL_FRect> focusedCaret() const { return searchFocused() ? search_.caret() : caret_; }
	std::optional<SDL_FRect> nodeRect(std::string_view surface, std::string_view id) const;
	std::optional<SDL_FRect> caretRect(std::string_view surface, std::string_view id) const;
	std::string dirtySummary() const;
	void setSystemPointer(bool enabled) { systemPointer_ = enabled; }

private:
	Tree& tree(const tsp::Surface& surface) { return *trees_.at(surface.id); }
	void dismissHover();
	int hoverWakeDelay() const;
	bool motion(float x, float y, bool refresh = false);
	bool click(float x, float y, std::string_view gesture);
	bool paintHover(const SDL_FRect& viewport);
	void openImageViewer(std::shared_ptr<const gfx::Image> image);
	void closeImageViewer();
	bool handleImageViewer(const SDL_Event& event, float sx, float sy);
	void sizeImageViewer();
	void clampImageViewer();
	void zoomImageViewer(float scale, float x, float y);
	SDL_FRect imageViewerRect() const;
	SDL_FRect imageViewerCloseRect() const;
	void imageViewerCursor(float x, float y);
	// Right-click menu: editing entries for an editor, else Copy/Select all plus the node's `actions.menu`.
	struct MenuEntry {
		enum class Kind { Cut, Copy, Paste, SelectAll, Action } kind;
		std::string label, shortcut, surface, node, action;
		bool enabled = true;
	};
	struct EditorRef {
		std::string surface, node;
	};
	std::optional<std::pair<std::reference_wrapper<Tree>, tsp::NodeId>> resolve(const EditorRef& ref);
	std::optional<std::pair<std::reference_wrapper<Tree>, tsp::NodeId>> focusedEditor();
	void selectEditor(const EditorRef& ref, std::optional<std::pair<size_t, size_t>> range);
	std::string editorSelectedText();
	void replaceEditorSelection(std::string_view text);
	void openMenu(float x, float y);
	std::optional<size_t> menuEntryAt(float x, float y) const;
	void runMenuEntry(const MenuEntry& entry);
	void paintMenu();
	tsp::Session& session_;
	std::reference_wrapper<SDL_Renderer> renderer_;
	std::reference_wrapper<text::Engine> engine_;
	const text::Fonts& fonts_;
	std::optional<gfx::ClipStack> clips_;
	int runningCount_ = 0;
	std::unordered_map<std::string, std::unique_ptr<Tree>> trees_;
	// Decoded `image` blobs, shared by every surface of this session (null: undecodable).
	std::unordered_map<std::string, std::shared_ptr<const gfx::Image>> images_;
	std::shared_ptr<const gfx::Image> image(std::string_view blob);
	std::unique_ptr<PointerState> pointer_;
	std::unique_ptr<ImageViewer> imageViewer_;
	std::vector<std::reference_wrapper<Tree>> painted_, kept_;
	std::optional<std::reference_wrapper<Tree>> active_, scrollOwner_;
	std::optional<SDL_FRect> caret_;
	SDL_FRect mainClip_{};
	float contentHeight_ = 0;
	Uint64 nextWake_ = 0;
	size_t lastDirty_ = 0, lastMeasured_ = 0, nodeCount_ = 0;
	bool systemPointer_ = true;
	// Selection endpoints in transcript coordinates (output x, y + main scroll): anchor, focus.
	std::optional<std::pair<SDL_FPoint, SDL_FPoint>> selection_;
	bool selecting_ = false, selectWord_ = false, gather_ = false;
	// The last paint animated something outside any tree leaf (hover card, tooltip timing).
	bool animatedElsewhere_ = false;
	SDL_FRect lastViewport_{};
	float mainScroll_ = 0;
	std::vector<MenuEntry> menu_;
	SDL_FRect menuRect_{};
	SDL_FRect menuPaintRect_{}, menuClip_{};
	gfx::Tween menuReveal_;
	std::optional<size_t> menuPress_;
	// The editor holding a terminal-side selection, and an in-progress press/drag in an editor.
	std::optional<EditorRef> editorSelection_;
	struct EditorDrag {
		EditorRef ref;
		size_t anchor;
		bool moved = false;
	};
	std::optional<EditorDrag> editorDrag_;
	// The transcript overview at the right edge (output pixels; empty when the column leaves no
	// room or nothing scrolls): its scale, how far it scrolls itself, and a slider drag (pointer
	// y and main scroll at the press).
	SDL_FRect minimap_{};
	SDL_FRect scrollbar_{}, scrollThumb_{};
	std::optional<std::pair<float, float>> scrollbarDrag_;
	bool scrollbarHover_ = false, scrollbarMarksDirty_ = true;
	std::vector<float> promptMarkers_;
	std::vector<Uint8> scrollbarRows_;
	float minimapScale_ = 0, minimapScroll_ = 0;
	std::optional<std::pair<float, float>> minimapDrag_;
	// Whether the last layout left the minimap its lane (only while the transcript scrolls).
	// Starts reserved: a long (resumed) transcript then lays out once at its final width, and
	// only a short one, which is cheap to lay out, needs the second pass without the lane.
	bool minimapLane_ = true;
	// Re-wrapping a long transcript at a new width can take hundreds of milliseconds, so while
	// the window is being resized the column keeps its laid-out width until the size settles.
	float laidWidth_ = -1, pendingWidth_ = -1, widthTransitionViewport_ = -1;
	Uint64 pendingSince_ = 0, relayoutAt_ = 0;
	bool slowRelayout_ = false;
	bool widthTransition_ = false, asideTransition_ = false, asideVisible_ = false, forceWidthLayout_ = false;
	bool transitioning_ = false, paintedOnce_ = false;
	std::vector<SDL_Vertex> minimapVertices_;
	std::vector<int> minimapIndices_;
	// The settings aside's left edge, draggable to resize it: the grab strip (empty without a
	// resizable aside), a drag in progress (pointer x and width at the press) and the chosen width
	// in pixels at scale 1, shared by every tab for the rest of the run.
	SDL_FRect asideGrip_{};
	float asideLayoutWidth_ = 0;
	std::optional<std::pair<float, float>> asideDrag_;
	static inline float asidePreferred_ = 400;
	void paintMinimap(float scroll, float maxScroll, float pad, float mainWidth);
	void paintScrollbar(float scroll, float maxScroll, float pad);
	Search search_;
	uint64_t searchRevision_ = 0;
	SDL_FRect searchGeometry_{};
	bool searchGather_ = false;
	void updateSearch();
	void revealSearchMatch();
	void scrollMainTo(float value);
};

} // namespace view
