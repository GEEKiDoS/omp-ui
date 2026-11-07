#include "title_bar.h"

#include "gfx/draw.h"
#include "text/spans.h"

#include <algorithm>
#include <array>
#include <utility>

namespace app {

void TitleBar::layout(const SDL_FRect& strip, float toOutputX, float toOutputY) {
	strip_ = strip;
	toOutputX_ = toOutputX;
	toOutputY_ = toOutputY;
}

SDL_FRect TitleBar::rect(Button button) const {
	// Buttons as wide as Windows' caption buttons are relative to their height (46 × 32).
	const float width = strip_.h * 1.45f, right = strip_.x + strip_.w;
	const int slot = button == Button::Close ? 1 : button == Button::Maximize ? 2 : 3;
	return {right - width * static_cast<float>(slot), strip_.y, width, strip_.h};
}

SDL_FRect TitleBar::titleArea() const {
	const auto first = rect(Button::Minimize);
	return {strip_.x, strip_.y, std::max(0.0f, first.x - strip_.x), strip_.h};
}

TitleBar::Button TitleBar::at(float x, float y) const {
	for (const auto button : {Button::Minimize, Button::Maximize, Button::Close}) {
		const auto box = rect(button);
		if (x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h)
			return button;
	}
	return Button::None;
}

void TitleBar::paint(SDL_Renderer& renderer, bool maximized, float em) const {
	const float glyph = em * 0.34f, stroke = std::max(1.0f, em / 15);
	for (const auto button : {Button::Minimize, Button::Maximize, Button::Close}) {
		const auto box = rect(button);
		const bool hot = hovered_ == button, down = hot && pressed_ == button;
		auto color = text::palette::muted;
		if (hot && button == Button::Close) {
			// The system caption's close red, in either theme.
			gfx::fill(renderer, box, down ? text::rgb(0x94231a) : text::rgb(0xc42b1c));
			color = text::rgb(0xffffff);
		} else if (hot) {
			gfx::fill(renderer, box, down ? text::palette::surface2 : text::palette::hover);
			color = text::palette::fg;
		}
		const SDL_FPoint c{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
		switch (button) {
		case Button::Minimize:
			gfx::line(renderer, {c.x - glyph, c.y}, {c.x + glyph, c.y}, color, stroke);
			break;
		case Button::Maximize:
			if (maximized) {
				// Restore: a square with another behind it.
				const float s = glyph * 0.8f, o = glyph * 0.35f;
				gfx::roundedBorder(renderer, {c.x - s, c.y - s + o, s * 2, s * 2}, stroke, color, stroke);
				gfx::line(renderer, {c.x - s + o, c.y - s}, {c.x + s + o, c.y - s}, color, stroke);
				gfx::line(renderer, {c.x + s + o, c.y - s}, {c.x + s + o, c.y + s - o}, color, stroke);
			} else
				gfx::roundedBorder(renderer, {c.x - glyph, c.y - glyph, glyph * 2, glyph * 2}, stroke, color, stroke);
			break;
		case Button::Close:
			gfx::line(renderer, {c.x - glyph, c.y - glyph}, {c.x + glyph, c.y + glyph}, color, stroke);
			gfx::line(renderer, {c.x - glyph, c.y + glyph}, {c.x + glyph, c.y - glyph}, color, stroke);
			break;
		case Button::None:
			break;
		}
	}
}

std::optional<TitleBar::Button> TitleBar::pointer(const SDL_Event& event, float x, float y, bool& dirty) {
	const auto target = at(x, y);
	switch (event.type) {
	case SDL_EVENT_MOUSE_MOTION:
		dirty |= hovered_ != target;
		hovered_ = target;
		// Hover only: other handlers still see the motion (e.g. to clear their own hover).
		return std::nullopt;
	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		dirty |= hovered_ != Button::None;
		hovered_ = Button::None;
		pressed_ = Button::None;
		return std::nullopt;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (target == Button::None || event.button.button != SDL_BUTTON_LEFT)
			return std::nullopt;
		pressed_ = target;
		dirty = true;
		return Button::None;
	case SDL_EVENT_MOUSE_BUTTON_UP: {
		if (pressed_ == Button::None || event.button.button != SDL_BUTTON_LEFT)
			return std::nullopt;
		const auto pressed = std::exchange(pressed_, Button::None);
		dirty = true;
		return pressed == target ? pressed : Button::None;
	}
	default:
		return std::nullopt;
	}
}

void TitleBar::install(SDL_Window& window) {
	SDL_SetWindowHitTest(&window, hitTest, this);
}

SDL_HitTestResult TitleBar::hitTest(SDL_Window* window, const SDL_Point* point, void* data) {
	const auto& bar = *static_cast<const TitleBar*>(data);
	int width = 0, height = 0;
	SDL_GetWindowSize(window, &width, &height);
	const bool maximized = (SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0;
	if (!maximized) {
		// Resize edges in window points.
		constexpr int edge = 6;
		const bool left = point->x < edge, right = point->x >= width - edge;
		const bool top = point->y < edge, bottom = point->y >= height - edge;
		if (top && left)
			return SDL_HITTEST_RESIZE_TOPLEFT;
		if (top && right)
			return SDL_HITTEST_RESIZE_TOPRIGHT;
		if (bottom && left)
			return SDL_HITTEST_RESIZE_BOTTOMLEFT;
		if (bottom && right)
			return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
		if (top)
			return SDL_HITTEST_RESIZE_TOP;
		if (bottom)
			return SDL_HITTEST_RESIZE_BOTTOM;
		if (left)
			return SDL_HITTEST_RESIZE_LEFT;
		if (right)
			return SDL_HITTEST_RESIZE_RIGHT;
	}
	const float x = static_cast<float>(point->x) * bar.toOutputX_, y = static_cast<float>(point->y) * bar.toOutputY_;
	const auto& strip = bar.strip_;
	if (!bar.modal_ && x >= strip.x && x < strip.x + strip.w && y >= strip.y && y < strip.y + strip.h
	    && bar.at(x, y) == Button::None)
		return SDL_HITTEST_DRAGGABLE;
	return SDL_HITTEST_NORMAL;
}

} // namespace app
