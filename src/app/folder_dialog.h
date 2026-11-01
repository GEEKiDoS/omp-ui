#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace app {

// Called on a dialog thread with the chosen folder (UTF-8), or nullopt when cancelled.
using FolderChosen = std::function<void(std::optional<std::string>)>;

// Shows the platform's folder picker without blocking the caller: on Windows the modern
// IFileOpenDialog on its own thread (SDL 3.2 uses the legacy SHBrowseForFolder tree),
// elsewhere SDL's dialog. Keep the returned thread until it finishes; it may be empty.
std::jthread pickFolder(SDL_Window& owner, const std::string& start, FolderChosen done);

} // namespace app
