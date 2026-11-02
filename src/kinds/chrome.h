#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"

namespace kinds {

// A stretch of editor source text shown verbatim in the visible runs (prompt, ghost and hidden
// decorations are not): maps a clicked byte in the runs back to the source.
struct EditorSpan {
	size_t visible, source, length;
};
// Returns the caret's byte offset in the visible, decorated runs.
// `selection` (UTF-16 from/to) overrides the node's anchor/cursor highlight with a terminal-side one.
size_t editorRuns(const tsp::Node& node, text::Runs& runs, const text::RunStyle& base, std::vector<EditorSpan>& spans,
                  std::optional<std::pair<size_t, size_t>> selection = std::nullopt);
// The UTF-16 source offset for a byte offset in the visible runs (clamped into the text).
size_t editorSourceOffset(const tsp::Node& node, const std::vector<EditorSpan>& spans, size_t visible);
SDL_Color cardFill(const tsp::Node& node);
SDL_Color cardBorder(const tsp::Node& node);
float toastOpacity(const tsp::Node& node, Uint64 arrived);
// Rewind marks: `pick` highlights a block from behind (painted before it), `drop` dims it (after).
void paintPick(SDL_Renderer& renderer, const SDL_FRect& rect, float em, float scale, bool previousPick, bool nextPick);
void paintDrop(SDL_Renderer& renderer, const SDL_FRect& rect, float scale);

} // namespace kinds
