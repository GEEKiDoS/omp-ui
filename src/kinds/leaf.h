#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"
#include "chrome.h"
#include "view/hit_targets.h"
#include "preedit.h"
#include "md.h"
#include "code.h"
#include "ansi.h"
#include "tool_agent.h"
#include "data.h"
#include "inline.h"
#include "list_tabs.h"
#include "prefs.h"
#include "picker.h"

namespace kinds {

struct LeafCache {
	text::Runs runs, label;
	text::Layout layout, labelLayout;
	text::LayoutOptions options;
	float width = -1;
	float padX = 0, padY = 0;
	float naturalWidth = 0;
	float textScrollX = 0, textScrollY = 0;
	bool expanded = false, collapsed = false;
	float opacity = 1;
	Preedit preedit;
	// Editors: visible-run ↔ source mapping, where the text was last drawn, and a terminal-side
	// selection (UTF-16 from/to) for select-all/cut/copy, which omp's editor has no notion of.
	std::vector<EditorSpan> editorSpans;
	SDL_FPoint textOrigin{};
	std::optional<std::pair<size_t, size_t>> editorSelection;
	// Editors: where the caret was last drawn and since when (it blinks for a while after moving).
	SDL_FRect caretRect{};
	Uint64 caretSince = 0;
	// `image` nodes: the decoded blob, supplied by the view (null until the blob arrives).
	std::shared_ptr<const gfx::Image> image;
	std::unique_ptr<MdCache> md;
	std::unique_ptr<CodeCache> code;
	std::unique_ptr<AnsiCache> ansi;
	std::unique_ptr<RowsCache> rows;
	std::unique_ptr<ToolAgentCache> toolAgent;
	std::unique_ptr<DataCache> data;
	std::unique_ptr<InlineCache> inlined;
	std::unique_ptr<ListTabsCache> listTabs;
	std::unique_ptr<PrefsCache> prefs;
	std::unique_ptr<PickerCache> picker;
};
struct LeafPaint {
	std::optional<SDL_FRect> caret;
	bool animating = false;
	Uint64 nextWake = 0;
	bool sizeChanged = false;
};

bool measured(tsp::Kind kind);
void sync(const tsp::Node& node, LeafCache& cache, text::Engine& engine, const text::Fonts& fonts, bool chromeless);
SDL_FPoint measure(const tsp::Node& node, LeafCache& cache, text::Engine& engine, const text::Fonts& fonts,
                   float width);
LeafPaint paint(const tsp::Node& node, LeafCache& cache, text::Engine& engine, const text::Fonts& fonts,
                SDL_Renderer& renderer, const SDL_FRect& rect, bool focused, view::HitTargets& hits);
void forget(LeafCache& cache, text::Engine& engine);

// One mark of the transcript overview (the minimap): a text line segment in its colour or a
// filled block (code background, picture), in leaf-local output pixels.
struct OverviewBar {
	SDL_FRect rect;
	SDL_Color color;
};
// Appends the leaf's text lines and blocks as last measured; `size` is its laid-out size.
void overview(const tsp::Node& node, const LeafCache& cache, const text::Fonts& fonts, SDL_FPoint size,
              std::vector<OverviewBar>& out);

} // namespace kinds
