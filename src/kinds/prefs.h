#pragma once

#include "core/tsp.h"
#include "text/inline_layout.h"
#include "view/hit_targets.h"

namespace kinds {

struct PrefsText {
	text::Runs runs;
	text::Layout layout;
	SDL_FRect rect{};
	bool body = false;
};
struct PrefsShape {
	SDL_FRect rect{};
	SDL_Color color{};
	float radius = 0;
	bool border = false, body = false;
};
struct PrefsHit {
	SDL_FRect rect{};
	view::TargetKind kind = view::TargetKind::Select;
	std::string item;
	std::optional<json> value;
	bool body = false;
};
struct PrefsCache {
	std::vector<PrefsText> texts;
	std::vector<PrefsShape> shapes;
	std::vector<PrefsHit> targets;
	float width = -1, headerHeight = 0, bodyHeight = 0;
	float scroll = 0, scrollMax = 0, viewportHeight = 0;
	float previewHeight = 0;
	SDL_FRect bodyRect{}, focusedRect{}, previewBox{}, optionRect{};
	std::optional<SDL_FRect> caret;
	bool caretBody = false, revealFocus = false;
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
