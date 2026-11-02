#pragma once

#include <filesystem>
#include <string>

namespace app {

// The look chosen at startup: the palette (applied to `text::palette`) plus window effects.
struct Theme {
	enum class Backdrop { None, Blur, Acrylic };
	enum class Fit { Cover, Contain, Tile, Center };

	std::string name = "dark";
	// Windows blurs the desktop behind the translucent parts (needs opacity < 1).
	Backdrop backdrop = Backdrop::None;
	// A picture drawn over the background, under the transcript.
	std::filesystem::path image;
	float imageOpacity = 0.15f;
	Fit fit = Fit::Cover;
	// OLED care: seconds without keyboard or mouse input in a window before everything but the
	// transcript column (sidebar, title bar, margins, minimap, prompt) fades as a vignette. 0: off.
	double dimAfter = 300;
	bool animations = true;
	// When omp signals a finished turn (or rings the bell) in a window that isn't in front: nothing,
	// the system sound, or a desktop notification (with the sound).
	enum class Notify { Silent, Sound, Notify };
	Notify notify = Notify::Notify;

	// The window needs a transparent framebuffer (translucent canvas or sidebar).
	bool transparent() const;
};

// Where config.json, themes/ and recent.json live: the per-user settings folder
// (%APPDATA%\omp\omp-ui\, ~/.local/share/omp/omp-ui/, ~/Library/Application Support/omp/omp-ui/).
// Empty when unknown.
std::filesystem::path settingsFolder();

// Chooses and applies the theme at startup, before any window exists.
//
// The theme is named by the OMP_UI_THEME environment variable, else by `"theme"` in
// `config.json` in the settings folder (which may also set `"dimAfter"`, `"animations"` and
// `"notify"`, see Theme). A name loads `themes/<name>.json` from that folder:
//   {
//     "base": "dark" | "light",
//     "colors": {"accent": "#2f5bd3", "canvas": "#17191fcc", …},
//     "window": {"opacity": 0.85, "panelOpacity": 0.92, "backdrop": "none" | "blur" | "acrylic"},
//     "background": {"image": "wallpaper.jpg", "opacity": 0.15, "fit": "cover" | "contain" | "tile" | "center"}
//   }
// `base` picks the built-in palette the colours override (and whether omp is told the terminal
// is dark). `window.opacity` scales the canvas and sidebar alpha, `window.panelOpacity` (default
// halfway between opacity and 1) the cards, code blocks, prompt and other surfaces drawn on it;
// `background.image` is relative to the themes folder. Without that file, `dark` and `light`
// are the built-in palettes.
// Problems are logged and fall back to the built-in parts.
//
// `userConfig` reads config.json and, when it is missing, creates it (`{"theme": "dark",
// "animations": true, "notify": "notify"}`) together with `themes/dark.json` and `themes/light.json` listing every
// setting, as starting points. Without it (hidden test runs, tsp-replay) only OMP_UI_THEME
// counts, and `dark`/`light` are always the built-ins. An explicit `configFile` is read even
// without `userConfig`, but never creates defaults there or in the user's settings folder.
Theme applyTheme(bool userConfig, const std::filesystem::path& configFile = {});

} // namespace app
