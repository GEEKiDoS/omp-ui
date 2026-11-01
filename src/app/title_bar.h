#pragma once

#include <SDL3/SDL.h>

#include <optional>

namespace app {

// The borderless window's caption: a strip across the top of the content area that drags the
// window (the OS also maximises on double-click and snaps it), and minimise / maximise /
// close buttons at its right end. Coordinates are output pixels.
class TitleBar {
public:
	enum class Button { None, Minimize, Maximize, Close };

	// `strip` is the caption area; `toOutputX/Y` convert window points to output pixels.
	void layout(const SDL_FRect& strip, float toOutputX, float toOutputY);
	const SDL_FRect& strip() const { return strip_; }
	// The area left for a title, between the strip's left edge and the buttons.
	SDL_FRect titleArea() const;
	void paint(SDL_Renderer& renderer, bool maximized, float em) const;
	// Mouse events at output pixel (x, y): nullopt when not ours; otherwise the button released
	// over the button it was pressed on (None for hover and press updates). Sets `dirty` when
	// the hover or press state changed.
	std::optional<Button> pointer(const SDL_Event& event, float x, float y, bool& dirty);
	// Routes OS hit testing (resize edges, caption drag) for `window` to this bar, which must
	// outlive the window.
	void install(SDL_Window& window);

private:
	SDL_FRect rect(Button button) const;
	Button at(float x, float y) const;
	static SDL_HitTestResult hitTest(SDL_Window* window, const SDL_Point* point, void* data);

	SDL_FRect strip_{};
	float toOutputX_ = 1, toOutputY_ = 1;
	Button hovered_ = Button::None, pressed_ = Button::None;
};

} // namespace app
