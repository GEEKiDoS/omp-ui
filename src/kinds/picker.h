#pragma once

#include "core/tsp.h"
#include "gfx/tween.h"
#include "text/inline_layout.h"
#include "view/hit_targets.h"
#include <array>

namespace kinds {

struct PickerText {
	text::Runs runs;
	text::Layout layout;
	float width = -1;
};
struct PickerFact {
	PickerText text;
	float x = 0, width = 0;
	float bar = -1;
};
struct PickerColumn {
	std::string id, format;
	PickerText head;
	float x = 0, width = 0;
	int priority = 0;
	bool visible = true;
};
struct PickerExtra {
	PickerText text;
	SDL_Color background{};
	float x = -1, width = 0;
	bool chip = false;
};
struct PickerRow {
	std::string id;
	size_t item = 0;
	float y = 0, height = 0, width = -1;
	bool group = false, disabled = false, current = false;
	PickerText label, detail;
	std::vector<PickerExtra> extras;
	std::vector<PickerFact> facts;
	int64_t second = -1;
	double clockOffset = 0;
};
struct PickerButton {
	std::string id, value;
	PickerText label, count;
	std::vector<PickerText> keys;
	SDL_FRect rect{};
	bool active = false, disabled = false, primary = false, danger = false, end = false;
};
struct PickerCache {
	PickerText title, subtitle, count, query, placeholder, message, stripLabel, confirmText, confirmLabel, close;
	std::vector<PickerButton> tabs, scopes, strip, actions;
	std::vector<PickerRow> rows;
	std::vector<PickerColumn> columns;
	std::array<double, 8> catalogueStamps{};
	std::string selected, confirmAct, shownFocus, shownScope;
	float scroll = 0, scopesScroll = 0, previewScroll = 0;
	gfx::Tween scrollTween, scopesScrollTween, previewScrollTween;
	float scrollMax = 0, scopesMax = 0, previewMax = 0, contentHeight = 0, scopesHeight = 0;
	float selectedTop = -1, selectedHeight = 0;
	float width = -1, height = -1, fontSize = 0, minimumHeight = 0;
	SDL_FRect head{}, closeBox{}, queryBox{}, tabsBox{}, columnsBox{}, scopesBox{}, listBox{}, previewBox{}, stripBox{},
	    stripContent{}, confirmBox{}, confirmButton{}, actionBox{}, painted{};
	std::optional<SDL_FRect> caret;
	size_t shown = 0, children = 0;
	int nextWake = -1;
	bool side = false, hasQuery = false, ready = false;
	bool revealSelection = true, transitioning = false;
};

void syncPicker(const tsp::Node& node, PickerCache& cache, text::Engine& engine, const text::Fonts& fonts);
void layoutPicker(const tsp::Node& node, PickerCache& cache, text::Engine& engine, const text::Fonts& fonts,
                  float width, float height);
bool paintPicker(const tsp::Node& node, PickerCache& cache, text::Engine& engine, const text::Fonts& fonts,
                 SDL_Renderer& renderer, const SDL_FRect& rect, bool focused, view::HitTargets& hits);
bool scrollPicker(PickerCache& cache, float x, float y, float delta);

} // namespace kinds
