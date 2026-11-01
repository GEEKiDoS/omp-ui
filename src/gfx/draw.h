#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <optional>
#include <vector>

namespace gfx {

// Insets and gaps in UI-font ems; Fonts::size() already includes display scale.
namespace space {
inline constexpr float xs = 0.25f;
inline constexpr float sm = 0.5f;
inline constexpr float md = 0.75f;
inline constexpr float lg = 1.0f;
inline constexpr float xl = 1.5f;
} // namespace space

void fill(SDL_Renderer& renderer, const SDL_FRect& rect, SDL_Color color);
// Darkens `bounds` outside `hole` (black at `alpha`), fading to nothing over `feather` towards
// the hole's edges: a vignette that leaves the hole itself untouched.
void shadeAround(SDL_Renderer& renderer, const SDL_FRect& bounds, const SDL_FRect& hole, float feather, Uint8 alpha);
void line(SDL_Renderer& renderer, SDL_FPoint from, SDL_FPoint to, SDL_Color color, float thickness = 1);
void rounded(SDL_Renderer& renderer, const SDL_FRect& rect, float radius, SDL_Color color);
void roundedBorder(SDL_Renderer& renderer, const SDL_FRect& rect, float radius, SDL_Color color, float thickness = 1);
void gradientRounded(SDL_Renderer& renderer, const SDL_FRect& rect, float radius,
                     const std::array<SDL_Color, 4>& colors);
void arc(SDL_Renderer& renderer, SDL_FPoint center, float radius, float start, float end, SDL_Color color,
         float thickness);
SDL_FRect intersect(const SDL_FRect& a, const SDL_FRect& b);
bool intersects(const SDL_FRect& a, const SDL_FRect& b);
class ClipStack {
public:
	explicit ClipStack(SDL_Renderer& renderer) : renderer_(renderer) {}
	void push(const SDL_FRect& rect);
	void pop();
	SDL_FRect current() const { return clips_.back(); }

private:
	SDL_Renderer& renderer_;
	std::vector<SDL_FRect> clips_;
	std::optional<SDL_Rect> outer_;
};
// Narrows the renderer clip to `rect` for a scope, restoring the previous clip afterwards.
// `visible` is the drawable part: bounded by the enclosing clip when one is set (a transcript
// gather pass sets one far beyond the output so off-screen text is reached), else by the output.
class ScopedClip {
public:
	ScopedClip(SDL_Renderer& renderer, const SDL_FRect& rect);
	~ScopedClip();
	ScopedClip(const ScopedClip&) = delete;
	ScopedClip& operator=(const ScopedClip&) = delete;
	SDL_FRect visible{};

private:
	SDL_Renderer& renderer_;
	SDL_Rect previous_{};
	bool enabled_;
};

} // namespace gfx
