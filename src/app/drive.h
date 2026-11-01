#pragma once

// Hidden, in-process GUI checks (never PostMessage, SendInput, cursor movement or
// activation): omp-ui --drive script.txt [-- omp ...]; tsp-replay fixture.jsonl
// --drive script.txt [--at frame]. One UTF-8 step per line; # lines are comments.
// wait ms | waitfor timeout-ms regex | text UTF-8 | key ctrl+c / enter+shift
// paste UTF-8 | move/click/dblclick/rclick/mclick x y | drag x1 y1 x2 y2 (left press, move, release)
// wheel x y lines | resize w h | drop path (omp-ui: a folder or file dropped on the window)
// scale factor | shot file.png | log file.txt | nodes file.json | reveal node-id | quit
// App tabs: tab cwd (active) | tabbg cwd (background) | tabcmd JSON | tabs file.json.
// tabcmd supplies cwd/argv/active; tabs dumps tab, pid and log metadata.
// App windows: window N | newwindow | merge | idle (start the OLED idle fade now)
// tabdrag fromWindow tabIndex targetWindow|new insertIndex (all indices 1-based).
// Text/paste recognize \\n, \\r, \\t and \\\\; other arguments are literal (no quotes).
// Coordinates and resize dimensions are content/output pixels, independent of DPI.
// waitfor searches inspector traffic and decoded surface properties; timeout or a
// bad step exits nonzero. nodes dumps IDs/properties plus painted rect/caret boxes;
// reveal scrolls the transcript so that node is at the top (browsing long recordings).
// shot reads the rendered frame immediately before it is presented: hidden SDL
// backbuffers need not survive Present. No screen capture or desktop interaction.
// Posted messages were unreliable: SDL reads real cursor/async modifier state;
// PrintWindow/DPI tricks and temporarily moving the user's cursor are forbidden.

#include <SDL3/SDL.h>
#include "core/tsp.h"

#include <functional>
#include <optional>
#include <regex>
#include <string_view>
#include <utility>

namespace view {
class Views;
}
namespace app {
class Input;

bool saveScreenshot(SDL_Renderer& renderer, const std::string& path);

class Drive {
public:
	Drive(const std::string& path, SDL_Window& window, SDL_Renderer& renderer, tsp::Session& session,
	      view::Views& views, std::function<void(float)> scale, std::function<void()> inspector, Input& input);
	void setTarget(tsp::Session& session, view::Views& views);
	void setWindow(SDL_Window& window, SDL_Renderer& renderer, Input& input);
	void setEventHandler(std::function<bool(const SDL_Event&)> handler) { eventHandler_ = std::move(handler); }
	void setTabAction(std::function<std::string(std::string_view, std::string_view)> action) {
		tabAction_ = std::move(action);
	}
	bool tick(bool& running);
	int wakeDelay() const;
	void beforePresent();
	const std::string& error() const { return error_; }

private:
	void fail(std::string message);
	bool matches() const;
	bool dump(const std::string& path, bool nodes);
	void pointer(Uint32 type, float x, float y, int button = 0, int clicks = 0, float lines = 0);
	std::reference_wrapper<SDL_Window> window_;
	std::reference_wrapper<SDL_Renderer> renderer_;
	std::reference_wrapper<tsp::Session> session_;
	std::reference_wrapper<view::Views> views_;
	std::function<void(float)> scale_;
	std::function<void()> inspector_;
	std::function<bool(const SDL_Event&)> eventHandler_;
	std::function<std::string(std::string_view, std::string_view)> tabAction_;
	std::reference_wrapper<Input> input_;
	std::vector<std::string> lines_;
	std::string path_, error_, pendingShot_;
	size_t next_ = 0;
	Uint64 deadline_ = 0;
	std::optional<std::regex> pattern_;
};
} // namespace app
