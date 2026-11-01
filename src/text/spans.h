#pragma once

#include "fonts.h"
#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace text {

constexpr SDL_Color rgb(unsigned value, Uint8 alpha = 255) {
	return {static_cast<Uint8>(value >> 16), static_cast<Uint8>(value >> 8), static_cast<Uint8>(value), alpha};
}

constexpr SDL_Color withAlpha(SDL_Color color, Uint8 alpha) {
	color.a = alpha;
	return color;
}

// The colour with its channels scaled by alpha, as a translucent window's framebuffer holds it.
constexpr SDL_Color premultiplied(SDL_Color color) {
	const auto scale = [&](Uint8 channel) {
		return static_cast<Uint8>((channel * color.a + 127) / 255);
	};
	return {scale(color.r), scale(color.g), scale(color.b), color.a};
}

// The theme's colours, set once by `usePalette` at startup before any window or text exists
// (nothing re-reads them later).
namespace palette {
inline SDL_Color bg, surface, surface2, border, fg, muted, dim, accent, info, success, warning, error, pending, user;
// Hovered sidebar row, user-turn card, selected/matched rows, focused field ring, primary,
// danger and caution buttons, and the strong mark (welcome logo).
inline SDL_Color hover, userCard, selected, focus, primary, danger, caution, mark;
// The window background behind the transcript and the sidebar's; an alpha below 255 makes the
// window translucent (theme `window.opacity` sets it).
inline SDL_Color canvas, sidebar;
} // namespace palette
// Whether the palette in use is dark (told to omp in `hello` and OSC 10/11 answers).
inline bool darkPalette = true;

void usePalette(bool dark);
// Palette entry names, as used by theme files and SVG variables, and an entry by name.
inline constexpr std::array<std::string_view, 24> paletteNames{
    "bg",       "surface", "surface2", "border", "fg",      "muted", "dim",    "accent",
    "info",     "success", "warning",  "error",  "pending", "user",  "hover",  "userCard",
    "selected", "focus",   "primary",  "danger", "caution", "mark",  "canvas", "sidebar"};
std::optional<std::reference_wrapper<SDL_Color>> paletteColor(std::string_view name);

struct RunStyle {
	FontId font = FontId::Ui;
	SDL_Color color = palette::fg;
	SDL_Color bg{0, 0, 0, 0};
	bool underline = false, strike = false, wavy = false, icon = false, inlineCode = false;
	bool chrome = false; // Line numbers and similar: drawn, never selected or copied.
	int fx = 0;          // 1 shimmer, 2 pulse
	std::string href;
};
struct Run {
	std::string text;
	RunStyle style;
};
using Runs = std::vector<Run>;
void applyTokens(std::string_view tokens, RunStyle& style);
Runs toRuns(const nlohmann::json& value, const RunStyle& base = {});
std::string plainText(const nlohmann::json& value);

} // namespace text
