#pragma once

#include "core/tsp.h"
#include "gfx/tween.h"
#include "text/inline_layout.h"
#include "view/hit_targets.h"

namespace kinds {

struct PrefsText {
	text::Runs runs;
	text::Layout layout;
	SDL_FRect rect{};
	bool body = false, popup = false;
};
struct PrefsShape {
	SDL_FRect rect{};
	SDL_Color color{};
	float radius = 0;
	bool border = false, body = false, popup = false;
};
struct PrefsHit {
	SDL_FRect rect{};
	view::TargetKind kind = view::TargetKind::Select;
	std::string item;
	std::optional<json> value;
	bool body = false, popup = false;
	// A choice pill: stays clickable over an open menu's backdrop, so another menu opens at once.
	bool pill = false;
};
// The open choice menu, floating at its pill like a native combobox. Its items are laid out in
// menu-local coordinates (`popup` set); paint places it below the pill, or above when there is
// more room there, and scrolls it to keep the highlighted option in view.
struct PrefsPopup {
	std::string row, selected;
	SDL_FRect anchor{}; // The pill, in body coordinates.
	float width = 0, height = 0, scroll = 0, viewportHeight = 0;
	float selectedTop = 0, selectedHeight = 0;
	gfx::Tween scrollTween, opening;
	SDL_FRect rect{}; // The revealed, moving menu bounds (output pixels).
	bool revealSelection = true;
};
struct PrefsCache {
	std::vector<PrefsText> texts;
	std::vector<PrefsShape> shapes;
	std::vector<PrefsHit> targets;
	float width = -1, headerHeight = 0, bodyHeight = 0;
	float scroll = 0, scrollMax = 0, viewportHeight = 0;
	gfx::Tween scrollTween;
	float previewHeight = 0;
	SDL_FRect bodyRect{}, focusedRect{}, previewBox{}, optionRect{};
	PrefsPopup popup;
	std::optional<SDL_FRect> caret;
	bool caretBody = false, revealFocus = false;
	bool painted = false, transitioning = false;
	std::string dragItem;
	std::optional<double> dragValue;
	std::string shownFocus, shownOption, page;
};

void syncPrefs(const tsp::Node& node, PrefsCache& cache, text::Engine& engine);
SDL_FPoint measurePrefs(const tsp::Node& node, PrefsCache& cache, text::Engine& engine, const text::Fonts& fonts,
                        float width);
std::optional<SDL_FRect> paintPrefs(const tsp::Node& node, PrefsCache& cache, text::Engine& engine,
                                    const text::Fonts& fonts, SDL_Renderer& renderer, const SDL_FRect& rect,
                                    bool focused, view::HitTargets& hits);
bool scrollPrefs(PrefsCache& cache, float x, float y, float delta);
double dragPrefsNumber(PrefsCache& cache, std::string_view item, const json& control, const SDL_FRect& track,
                       float startX, float x, float scale);

} // namespace kinds
