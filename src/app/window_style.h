#pragma once

#include "config.h"

#include <SDL3/SDL.h>

#include <string_view>

namespace app {

// Creation flags for omp-ui's own caption (title_bar.h): borderless where the window system
// allows nothing better; 0 on Windows, where styleWindow removes the frame instead.
SDL_WindowFlags frameFlags();

// Window-system styling. On Windows: a dark or light frame, the system frame removed while
// keeping the shadow, rounded corners, snapping and animations, and the theme's material
// (`backdrop`) behind a translucent window (`translucent`: created with an alpha framebuffer).
// Safe to call again after a config reload. Elsewhere a no-op.
void styleWindow(SDL_Window& window, const Theme& theme, bool translucent);

// Draws attention to a window in the back after omp finished a turn or rang the bell: the system
// notification sound, and with `toast` a desktop notification (Windows: from a tray icon that
// lives for the window's lifetime; clicking it raises the window) titled `title` saying `message`.
// The taskbar button flashes either way.
void notify(SDL_Window& window, bool toast, std::string_view title, std::string_view message);

} // namespace app
