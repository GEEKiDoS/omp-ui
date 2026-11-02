#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "drive.h"
#include "input.h"
#include "tabs.h"
#include "gfx/icons.h"
#include "folder_dialog.h"
#include "core/text_util.h"
#include "config.h"
#include "window_style.h"
#include "title_bar.h"
#include "composition.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
struct SdlLifetime {
	~SdlLifetime() { SDL_Quit(); }
};
struct TtfLifetime {
	~TtfLifetime() { TTF_Quit(); }
};
struct WindowDeleter {
	void operator()(SDL_Window* value) const { SDL_DestroyWindow(value); }
};
struct RendererDeleter {
	void operator()(SDL_Renderer* value) const { SDL_DestroyRenderer(value); }
};
struct TextureDeleter {
	void operator()(SDL_Texture* value) const { SDL_DestroyTexture(value); }
};
// Animation areas for one partial repaint: overlapping or nearby rects (within a line or so)
// merge, and many scattered ones collapse into their bounding box, so a frame repaints a few
// regions at most.
std::vector<SDL_FRect> damageRegions(std::vector<SDL_FRect> rects) {
	constexpr float slack = 24;
	const auto unite = [](const SDL_FRect& a, const SDL_FRect& b) {
		const float left = std::min(a.x, b.x), top = std::min(a.y, b.y);
		return SDL_FRect{left, top, std::max(a.x + a.w, b.x + b.w) - left, std::max(a.y + a.h, b.y + b.h) - top};
	};
	const auto near = [](const SDL_FRect& a, const SDL_FRect& b) {
		return a.x - slack < b.x + b.w && b.x - slack < a.x + a.w && a.y - slack < b.y + b.h && b.y - slack < a.y + a.h;
	};
	std::erase_if(rects, [](const SDL_FRect& rect) { return rect.w <= 0 || rect.h <= 0; });
	for (bool merged = true; merged;) {
		merged = false;
		for (size_t i = 0; i < rects.size() && !merged; ++i)
			for (size_t j = i + 1; j < rects.size(); ++j)
				if (near(rects[i], rects[j])) {
					rects[i] = unite(rects[i], rects[j]);
					rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(j));
					merged = true;
					break;
				}
	}
	if (rects.size() > 4) {
		for (size_t i = 1; i < rects.size(); ++i)
			rects[0] = unite(rects[0], rects[i]);
		rects.resize(1);
	}
	return rects;
}
bool showErrors = true;
// A right-click menu in the sidebar. Folder rows show the name and the dimmed full path.
struct SidebarMenu {
	enum class Kind {
		PickFolder, // New session in a folder chosen with the picker.
		NewSession, // New session in `folder`.
		Heading,
		ShowFolder, // Open `folder` in the system file manager.
		CloseTab,
		ReloadConfig, // Re-read config.json and the theme.
	};
	struct Entry {
		Kind kind;
		std::string label, folder;
		size_t tab = 0;
		bool showPath = false; // Recent folders: the name, then the full path dimmed.
	};
	SDL_FRect rect;
	std::vector<Entry> entries;
	std::optional<size_t> hovered;
};

// A `file://` URL for a local folder, as SDL_OpenURL takes it on every platform.
std::string fileUrl(const std::filesystem::path& folder) {
	const auto generic = folder.generic_u8string();
	std::string url = generic.starts_with(u8"/") ? "file://" : "file:///";
	for (const char8_t unit : generic) {
		const auto ch = static_cast<unsigned char>(unit);
		if (std::isalnum(ch) || std::string_view("/-._~:").find(static_cast<char>(ch)) != std::string_view::npos)
			url += static_cast<char>(ch);
		else
			url += std::format("%{:02X}", ch);
	}
	return url;
}

struct Window {
	// Before `window`: the window's hit test reads it until the window is destroyed.
	app::TitleBar titleBar;
	std::unique_ptr<SDL_Window, WindowDeleter> window;
	std::unique_ptr<SDL_Renderer, RendererDeleter> renderer;
	app::Composition composition; // Translucent themes on Windows; released before the renderer.
	// The last frame (one window-sized texture), so animation frames repaint only `damage`: the
	// areas animated last time, or nullopt when unknown (a full repaint).
	std::unique_ptr<SDL_Texture, TextureDeleter> frame;
	std::optional<std::vector<SDL_FRect>> damage;
	Uint64 lastPaint = 0;
	bool tooltipPending = false;
	// OLED care: the last keyboard/mouse input, and when the idle fade began (0: not dimmed).
	Uint64 lastInput = SDL_GetTicks();
	Uint64 dimmedAt = 0;
	// A timed leaf's frame is due: repainted partially when its area is known.
	bool due = false;
	void wake(view::Views::Wake what) {
		dirty |= what == view::Views::Wake::Full;
		due |= what == view::Views::Wake::Animation;
	}
	std::unique_ptr<text::Engine> engine;
	std::vector<std::unique_ptr<app::Tab>> tabs;
	std::unique_ptr<app::Input> input;
	app::Sidebar sidebar;
	size_t active = 0;
	int outputW = 1, outputH = 1, windowW = 1, windowH = 1;
	float toOutputX = 1, toOutputY = 1;
	SDL_FRect content{0, 0, 1, 1}, restart{}, openWorkspace{};
	bool inspector = false, dirty = true, animating = false, choosingFolder = false;
	// Created with a translucent framebuffer (fixed for the window's lifetime), and a note shown
	// in the caption when a reloaded theme needs what this window lacks.
	bool translucent = false;
	std::string notice;
	std::optional<SidebarMenu> menu;
	std::optional<app::SidebarHit> sidebarPress;
	std::optional<Uint64> switchStarted;
	double lastSwitchMs = 0;
	uint64_t switchCount = 0, paintCount = 0, frontOrder = 0;
	SDL_WindowID id() const { return SDL_GetWindowID(window.get()); }
};
struct FolderReply {
	SDL_WindowID window;
	std::optional<std::string> path;
};
struct FolderInbox {
	std::mutex mutex;
	std::vector<FolderReply> replies;
	Uint32 event = 0;
};
int fail(std::string_view error) {
	SDL_Log("%.*s", static_cast<int>(error.size()), error.data());
	if (showErrors)
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "omp-ui", std::string(error).c_str(), nullptr);
	return 1;
}
std::vector<std::string> commandLine(int argc, char* argv[]) {
	std::vector<std::string> command;
	for (int i = 1; i < argc; ++i)
		if (std::string_view(argv[i]) == "--") {
			for (++i; i < argc; ++i)
				command.emplace_back(argv[i]);
			break;
		}
	if (command.empty())
		command.emplace_back("omp");
	return command;
}
SDL_WindowID eventWindow(const SDL_Event& event) {
	if (const auto window = SDL_GetWindowFromEvent(&event))
		return SDL_GetWindowID(window);
	return 0;
}
enum class ActionKind { New, Close, Select, Next, Previous, Toggle, Inspector, Restart, Split, Merge };
struct Action {
	ActionKind kind;
	SDL_WindowID window;
	size_t index = 0;
};
struct TabDrag {
	SDL_WindowID window;
	size_t index;
	SDL_FPoint start, point;
	bool moved = false;
};
} // namespace

int main(int argc, char* argv[]) {
	std::string drivePath, workspace;
	for (int i = 1; i < argc && std::string_view(argv[i]) != "--"; ++i) {
		if (std::string_view(argv[i]) == "--drive") {
			showErrors = false;
			if (++i == argc || !*argv[i])
				return fail("--drive requires a script path");
			drivePath = argv[i];
		} else if (workspace.empty())
			workspace = argv[i];
	}
	std::error_code filesystemError;
	if (workspace.empty() || !std::filesystem::is_directory(text::utf8Path(workspace), filesystemError))
		workspace.clear();
	if (!drivePath.empty() && workspace.empty())
		return fail("--drive requires an existing workspace folder argument");
	FolderInbox folders;
	std::vector<std::jthread> folderDialogs; // Joined after SDL shutdown closes their owner windows.
	// SDL keeps the display awake and the screensaver off by default (meant for games); a terminal
	// must let the system blank and sleep the screen.
	SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return fail(SDL_GetError());
	auto theme = app::applyTheme(drivePath.empty()); // Hidden test runs never create the user's config.
	// The theme's background picture, decoded once and drawn in every window.
	std::optional<gfx::Image> wallpaper;
	const auto loadWallpaper = [&] {
		wallpaper.reset();
		if (theme.image.empty())
			return;
		std::ifstream input(theme.image, std::ios::binary);
		const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		wallpaper = gfx::Image::decode(bytes);
		if (!wallpaper)
			SDL_Log("Theme %s: cannot read background image %s", theme.name.c_str(), theme.image.string().c_str());
	};
	loadWallpaper();
	SdlLifetime sdl;
	if (!TTF_Init())
		return fail(SDL_GetError());
	TtfLifetime ttf;
	text::Fonts fonts;
	float scale = 1;
	std::optional<float> forcedScale = []() -> std::optional<float> {
		if (const auto value = SDL_getenv("OMP_UI_SCALE"))
			return std::max(0.5f, static_cast<float>(std::atof(value)));
		return {};
	}();
	if (forcedScale)
		scale = *forcedScale;
	std::string error;
	if (!fonts.load(scale, error))
		return fail(error);
	float monoAdvance = fonts.monoAdvance(), lineHeight = fonts.lineHeight();
	const Uint32 wakeEvent = SDL_RegisterEvents(1);
	folders.event = wakeEvent;
	const auto command = commandLine(argc, argv);
	app::RecentFolders recent(drivePath.empty()); // Hidden test runs must not rewrite the user's list.
	std::vector<std::unique_ptr<Window>> windows;
	SDL_WindowID targetWindow = 0;
	std::unique_ptr<app::Drive> drive;
	std::vector<Action> actions;
	std::optional<TabDrag> drag;
	bool running = true;
	uint64_t frontOrder = 0;

	auto findWindow = [&](SDL_WindowID id) -> std::optional<std::reference_wrapper<Window>> {
		for (auto& window : windows)
			if (window->id() == id)
				return *window;
		return std::nullopt;
	};
	auto cancelDrag = [&] {
		if (!drag)
			return;
		if (drivePath.empty())
			SDL_CaptureMouse(false);
		drag.reset();
		for (auto& window : windows)
			window->sidebarPress.reset();
	};
	auto tabMetrics = [&](Window& window, app::Tab& tab) {
		// A protocol aside docks independently per tab, after the application sidebar.
		const float width = tab.views.contentWidth(window.content.w);
		const int cols = std::max(1, static_cast<int>(std::floor(width / monoAdvance)));
		const int rows = std::max(1, static_cast<int>(std::floor(window.content.h / lineHeight)));
		if (cols != tab.host.session.cols || rows != tab.host.rows)
			tab.pty.resize(cols, rows);
		tab.host.rows = rows;
		tab.host.session.resize(cols, monoAdvance, lineHeight);
	};
	auto resize = [&](Window& window) {
		SDL_GetWindowSize(window.window.get(), &window.windowW, &window.windowH);
		SDL_GetRenderOutputSize(window.renderer.get(), &window.outputW, &window.outputH);
		window.toOutputX = static_cast<float>(window.outputW) / std::max(1, window.windowW);
		window.toOutputY = static_cast<float>(window.outputH) / std::max(1, window.windowH);
		const float left =
		    std::min(window.sidebar.width(fonts), std::max(0.0f, static_cast<float>(window.outputW) - 1));
		// omp-ui draws the caption: a strip as tall as the sidebar header above the content.
		const float caption = std::min(window.sidebar.headerHeight(fonts), static_cast<float>(window.outputH) - 1);
		const float width = std::max(1.0f, window.outputW - left);
		window.titleBar.layout({left, 0, width, caption}, window.toOutputX, window.toOutputY);
		window.content = {left, caption, width, std::max(1.0f, window.outputH - caption)};
		for (auto& tab : window.tabs)
			tabMetrics(window, *tab);
		if (!window.tabs.empty())
			window.sidebar.reveal(window.active, fonts, static_cast<float>(window.outputH), window.tabs.size());
		window.dirty = true;
	};
	auto setScale = [&](float value) {
		if (value == scale)
			return;
		scale = value;
		for (auto& window : windows)
			window->engine->clear();
		fonts.setScale(scale);
		monoAdvance = fonts.monoAdvance();
		lineHeight = fonts.lineHeight();
		for (auto& window : windows) {
			for (auto& tab : window->tabs)
				tab->views.metricsChanged();
			resize(*window);
		}
	};
	auto createWindow = [&](std::optional<SDL_Point> position = std::nullopt) -> SDL_WindowID {
		auto window = std::make_unique<Window>();
		// Open at three quarters of the primary display's usable area (window units).
		int width = 1280, height = 860;
		if (SDL_Rect usable; SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
			width = std::max(width, usable.w * 3 / 4);
			height = std::max(height, usable.h * 3 / 4);
			width = std::min(width, usable.w);
			height = std::min(height, usable.h);
		}
		window->window.reset(SDL_CreateWindow(
		    "omp-ui", width, height,
		    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
		        | (theme.transparent() ? app::Composition::windowFlags() : SDL_WindowFlags{0}) | app::frameFlags()
		        | (drivePath.empty() ? 0 : SDL_WINDOW_HIDDEN | SDL_WINDOW_NOT_FOCUSABLE)));
		if (!window->window) {
			error = SDL_GetError();
			return 0;
		}
		window->titleBar.install(*window->window); // Window is heap-allocated, so the bar's address is stable.
		if (position)
			SDL_SetWindowPosition(window->window.get(), position->x, position->y);
		window->renderer.reset(SDL_CreateRenderer(window->window.get(), nullptr));
		if (!window->renderer) {
			error = SDL_GetError();
			return 0;
		}
		SDL_SetRenderVSync(window->renderer.get(), 1); // A composition (below) moves vsync to its own chain.
		app::styleWindow(*window->window, theme, theme.transparent());
		// SDL's Direct3D 11 swap chain composes opaque; a translucent theme presents through
		// DirectComposition instead (a no-op elsewhere, where SDL_WINDOW_TRANSPARENT suffices).
		// After the frame change, so the composition binds to the window's final shape.
		window->translucent = theme.transparent() && window->composition.attach(*window->window, *window->renderer);
		if (theme.transparent() && !window->translucent) {
			SDL_Log("Translucent window unavailable; drawing it opaque");
			app::styleWindow(*window->window, theme, false);
		}
		SDL_SetRenderDrawBlendMode(window->renderer.get(), SDL_BLENDMODE_BLEND);
		window->engine = std::make_unique<text::Engine>(*window->renderer, fonts);
		if (!window->engine->valid()) {
			error = SDL_GetError();
			return 0;
		}
		const auto id = window->id();
		window->frontOrder = ++frontOrder;
		windows.push_back(std::move(window));
		resize(*windows.back());
		return id;
	};
	targetWindow = createWindow();
	if (!targetWindow)
		return fail(error);
	if (!forcedScale)
		setScale(SDL_GetWindowDisplayScale(windows.front()->window.get()));

	auto retarget = [&](Window& window) {
		if (window.tabs.empty())
			return;
		auto& tab = *window.tabs[window.active];
		if (!window.input) {
			window.input = std::make_unique<app::Input>(*window.window, tab.host, tab.pty);
			const auto id = window.id();
			window.input->onShortcut = [&, id](const SDL_KeyboardEvent& key) {
				const auto found = findWindow(id);
				if (!found || found->get().tabs.empty())
					return false;
				auto& current = found->get();
				if (current.input->claimed(key.key, key.mod)) {
					auto& views = current.tabs[current.active]->views;
					if (key.key == SDLK_C)
						views.copySelection();
					else if (key.key == SDLK_X)
						views.cutSelection();
					else
						views.selectAll();
					current.dirty = true;
					return true;
				}
				std::optional<ActionKind> kind;
				size_t index = 0;
				const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
				if (key.key == SDLK_F12)
					kind = ActionKind::Inspector;
				else if ((key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER) && current.tabs[current.active]->exited)
					kind = ActionKind::Restart;
#ifdef __APPLE__
				else if ((key.mod & SDL_KMOD_GUI) && !(key.mod & SDL_KMOD_CTRL)) {
					if (shift && (key.mod & SDL_KMOD_ALT) && key.key == SDLK_T)
						kind = ActionKind::Split;
					else if (shift && (key.mod & SDL_KMOD_ALT) && key.key == SDLK_M)
						kind = ActionKind::Merge;
					else if (!(key.mod & SDL_KMOD_ALT)) {
						if (!shift && key.key == SDLK_T)
							kind = ActionKind::New;
						else if (!shift && key.key == SDLK_W)
							kind = ActionKind::Close;
						else if (!shift && key.key == SDLK_B)
							kind = ActionKind::Toggle;
						else if (shift && key.key == SDLK_LEFTBRACKET)
							kind = ActionKind::Previous;
						else if (shift && key.key == SDLK_RIGHTBRACKET)
							kind = ActionKind::Next;
						else if (!shift && key.key >= SDLK_1 && key.key <= SDLK_9) {
							kind = ActionKind::Select;
							index = key.key - SDLK_1;
						}
					}
				}
#else
				else if ((key.mod & SDL_KMOD_CTRL) && !(key.mod & SDL_KMOD_GUI)) {
					if (shift && (key.mod & SDL_KMOD_ALT) && key.key == SDLK_T)
						kind = ActionKind::Split;
					else if (shift && (key.mod & SDL_KMOD_ALT) && key.key == SDLK_M)
						kind = ActionKind::Merge;
					else if (!(key.mod & SDL_KMOD_ALT)) {
						if (shift && key.key == SDLK_T)
							kind = ActionKind::New;
						else if (shift && key.key == SDLK_W)
							kind = ActionKind::Close;
						else if (shift && key.key == SDLK_B)
							kind = ActionKind::Toggle;
						else if (key.key == SDLK_TAB)
							kind = shift ? ActionKind::Previous : ActionKind::Next;
						else if (!shift && key.key == SDLK_PAGEUP)
							kind = ActionKind::Previous;
						else if (!shift && key.key == SDLK_PAGEDOWN)
							kind = ActionKind::Next;
						else if (shift && key.key >= SDLK_1 && key.key <= SDLK_9) {
							kind = ActionKind::Select;
							index = key.key - SDLK_1;
						}
					}
				}
#endif
				if (!kind)
					return false;
				actions.push_back({*kind, id, index});
				return true;
			};
			window.input->claimKey = [&, id](SDL_Keycode key, SDL_Keymod mod) {
			// Copy/cut only take the key while something is selected; otherwise it belongs to the program (Ctrl+C).
#ifdef __APPLE__
				const bool command = (mod & SDL_KMOD_GUI) && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT));
#else
				const bool command = (mod & SDL_KMOD_CTRL) && !(mod & (SDL_KMOD_GUI | SDL_KMOD_ALT | SDL_KMOD_SHIFT));
#endif
				const auto found = findWindow(id);
				if (!command || !found || found->get().tabs.empty())
					return false;
				const auto& views = found->get().tabs[found->get().active]->views;
				return (key == SDLK_C && views.hasSelection()) || (key == SDLK_X && views.hasEditorSelection())
				    || key == SDLK_A;
			};
			window.input->editKey = [&, id](SDL_Keycode key, SDL_Keymod mod, std::string_view typed) {
				const auto found = findWindow(id);
				if (!found || found->get().tabs.empty())
					return false;
				const bool consumed = found->get().tabs[found->get().active]->views.editorKey(key, mod, typed);
				found->get().dirty = true;
				return consumed;
			};
		} else
			window.input->setTarget(tab.host, tab.pty);
		window.input->setEnabled(!tab.exited);
		SDL_SetWindowTitle(window.window.get(), (tab.title + " — omp-ui").c_str());
		if (drive && targetWindow == window.id()) {
			drive->setWindow(*window.window, *window.renderer, *window.input);
			drive->setTarget(tab.host.session, tab.views);
		}
	};
	auto target = [&](SDL_WindowID id) {
		targetWindow = id;
		if (const auto found = findWindow(id))
			retarget(found->get());
	};
	auto addTab = [&](Window& window, const std::string& cwd, std::vector<std::string> argv, bool activate) {
		cancelDrag();
		recent.add(cwd);
		auto tab = std::make_unique<app::Tab>(cwd, std::string{}, std::move(argv), wakeEvent, *window.renderer,
		                                      *window.engine, fonts);
		tab->views.setSystemPointer(drivePath.empty());
		tabMetrics(window, *tab);
		error.clear();
		tab->start(tab->host.session.cols, tab->host.rows, error);
		if (!error.empty())
			SDL_Log("Tab start: %s", error.c_str());
		const bool first = window.tabs.empty();
		if (!first && activate) {
			window.tabs[window.active]->setVisible(false);
			window.engine->clear();
			window.switchStarted = SDL_GetPerformanceCounter();
			++window.switchCount;
		}
		window.tabs.push_back(std::move(tab));
		if (first || activate)
			window.active = window.tabs.size() - 1;
		window.tabs.back()->setVisible(first || activate);
		retarget(window);
		window.sidebar.reveal(window.active, fonts, static_cast<float>(window.outputH), window.tabs.size());
		window.dirty = true;
	};
	// A dropped folder opens a session there; a dropped file pastes its path into the prompt,
	// quoted when it has spaces, with a separating space for multi-file drops.
	auto dropped = [&](Window& window, const std::string& path) {
		std::error_code error;
		if (std::filesystem::is_directory(text::utf8Path(path), error))
			addTab(window, path, command, true);
		else if (!window.tabs.empty() && !window.tabs[window.active]->exited)
			window.input->paste((path.find(' ') == std::string::npos ? path : "\"" + path + "\"") + " ");
		window.dirty = true;
	};
	auto selectTab = [&](Window& window, size_t index) {
		if (index >= window.tabs.size() || index == window.active)
			return;
		window.switchStarted = SDL_GetPerformanceCounter();
		++window.switchCount;
		window.tabs[window.active]->setVisible(false);
		window.engine->clear(); // Documents, Yoga and scroll remain owned by the tab.
		window.active = index;
		window.tabs[index]->setVisible(true);
		retarget(window);
		window.sidebar.reveal(index, fonts, static_cast<float>(window.outputH), window.tabs.size());
		window.dirty = true;
	};
	auto closeWindow = [&](SDL_WindowID id) {
		cancelDrag();
		const auto found =
		    std::find_if(windows.begin(), windows.end(), [&](const auto& window) { return window->id() == id; });
		if (found == windows.end())
			return;
		if (targetWindow == id && windows.size() > 1)
			target((*windows.begin())->id() == id ? windows[1]->id() : windows.front()->id());
		windows.erase(found); // Tab destructors close and join their own processes.
		if (windows.empty())
			running = false;
	};
	auto closeTab = [&](Window& window, size_t index) {
		if (index >= window.tabs.size())
			return;
		cancelDrag();
		// The last tab of the last window leaves an empty window offering "Open workspace"; other windows close.
		if (window.tabs.size() == 1 && windows.size() > 1) {
			closeWindow(window.id());
			return;
		}
		if (window.tabs.size() == 1) {
			window.input.reset(); // It refers to the closing tab's host and pty.
			window.tabs.clear();
			window.active = 0;
			window.sidebar.hovered.reset();
			window.dirty = true;
			return;
		}
		if (index == window.active)
			selectTab(window, index + 1 < window.tabs.size() ? index + 1 : index - 1);
		window.tabs.erase(window.tabs.begin() + static_cast<std::ptrdiff_t>(index));
		if (index < window.active)
			--window.active;
		window.sidebar.hovered.reset();
		window.sidebar.reveal(window.active, fonts, static_cast<float>(window.outputH), window.tabs.size());
		window.dirty = true;
	};
	auto restart = [&](Window& window) {
		auto previous = std::move(window.tabs[window.active]);
		auto tab = std::make_unique<app::Tab>(previous->cwd, previous->title, previous->command, wakeEvent,
		                                      *window.renderer, *window.engine, fonts);
		tab->views.setSystemPointer(drivePath.empty());
		tabMetrics(window, *tab);
		error.clear();
		tab->start(tab->host.session.cols, tab->host.rows, error);
		window.tabs[window.active] = std::move(tab);
		window.tabs[window.active]->setVisible(true);
		retarget(window);
		window.dirty = true;
	};
	auto chooseFolder = [&](Window& window) {
		if (window.choosingFolder)
			return;
		if (!drivePath.empty()) {
			SDL_Log("Native workspace dialogs are not driven; use the tab step");
			return;
		}
		window.choosingFolder = true;
		const auto cwd = window.tabs.empty() ? app::currentFolder() : window.tabs[window.active]->cwd;
		folderDialogs.push_back(
		    app::pickFolder(*window.window, cwd, [&folders, id = window.id()](std::optional<std::string> path) {
			    {
				    std::scoped_lock lock(folders.mutex);
				    folders.replies.push_back({id, std::move(path)});
			    }
			    SDL_Event event{};
			    event.type = folders.event;
			    SDL_PushEvent(&event);
		    }));
	};
	// New tab: same folder as the active session; only an empty window has to ask.
	auto newTab = [&](Window& window) {
		if (window.tabs.empty())
			chooseFolder(window);
		else
			addTab(window, window.tabs[window.active]->cwd, command, true);
	};
	auto moveTab = [&](SDL_WindowID from, size_t index, SDL_WindowID to, size_t insertion, bool activate) {
		cancelDrag();
		const auto sourceFound = findWindow(from), destinationFound = findWindow(to);
		if (!sourceFound || !destinationFound)
			return;
		auto& source = sourceFound->get();
		auto& destination = destinationFound->get();
		if (index >= source.tabs.size())
			return;
		insertion = std::min(insertion, destination.tabs.size());
		if (from == to) {
			if (insertion > index)
				--insertion;
			if (insertion == index)
				return;
			const size_t selected = source.active;
			auto tab = std::move(source.tabs[index]);
			source.tabs.erase(source.tabs.begin() + static_cast<std::ptrdiff_t>(index));
			source.tabs.insert(source.tabs.begin() + static_cast<std::ptrdiff_t>(insertion), std::move(tab));
			if (selected == index)
				source.active = insertion;
			else {
				source.active = selected - (index < selected ? 1 : 0);
				if (insertion <= source.active)
					++source.active;
			}
			source.dirty = true;
			return;
		}
		auto tab = std::move(source.tabs[index]);
		tab->setVisible(false);
		tab->views.rebind(*destination.renderer, *destination.engine);
		source.tabs.erase(source.tabs.begin() + static_cast<std::ptrdiff_t>(index));
		if (!source.tabs.empty()) {
			if (index < source.active)
				--source.active;
			else if (index == source.active)
				source.active = std::min(index, source.tabs.size() - 1);
			source.tabs[source.active]->setVisible(true);
			retarget(source);
			source.dirty = true;
		}
		const bool first = destination.tabs.empty();
		if (!first && activate)
			destination.tabs[destination.active]->setVisible(false);
		if (!first && insertion <= destination.active)
			++destination.active;
		destination.tabs.insert(destination.tabs.begin() + static_cast<std::ptrdiff_t>(insertion), std::move(tab));
		if (first || activate)
			destination.active = insertion;
		destination.tabs[insertion]->setVisible(first || activate);
		tabMetrics(destination, *destination.tabs[insertion]);
		retarget(destination);
		destination.sidebar.reveal(destination.active, fonts, static_cast<float>(destination.outputH),
		                           destination.tabs.size());
		destination.dirty = true;
		if (activate || targetWindow == from)
			target(to);
		if (source.tabs.empty())
			closeWindow(from);
	};
	auto splitTab = [&](Window& window, std::optional<SDL_Point> position = std::nullopt) {
		if (window.tabs.empty())
			return;
		if (!position) {
			int x = 0, y = 0;
			SDL_GetWindowPosition(window.window.get(), &x, &y);
			position = SDL_Point{x + 40, y + 40};
		}
		const auto id = createWindow(position);
		if (!id) {
			SDL_Log("Cannot create window: %s", error.c_str());
			return;
		}
		moveTab(window.id(), window.active, id, 0, true);
	};
	auto mergeWindows = [&](SDL_WindowID id) {
		for (;;) {
			const auto other =
			    std::find_if(windows.begin(), windows.end(), [&](const auto& window) { return window->id() != id; });
			if (other == windows.end())
				break;
			if ((*other)->tabs.empty()) {
				closeWindow((*other)->id());
				continue;
			}
			const auto destination = findWindow(id);
			moveTab((*other)->id(), 0, id, destination->get().tabs.size(), false);
		}
		target(id);
	};
	auto processActions = [&] {
		if (!actions.empty())
			cancelDrag();
		while (!actions.empty()) {
			const Action action = actions.front();
			actions.erase(actions.begin());
			const auto found = findWindow(action.window);
			if (!found || found->get().tabs.empty())
				continue;
			auto& window = found->get();
			switch (action.kind) {
			case ActionKind::New:
				newTab(window);
				break;
			case ActionKind::Close:
				closeTab(window, window.active);
				break;
			case ActionKind::Select:
				selectTab(window, action.index);
				break;
			case ActionKind::Next:
				selectTab(window, (window.active + 1) % window.tabs.size());
				break;
			case ActionKind::Previous:
				selectTab(window, (window.active + window.tabs.size() - 1) % window.tabs.size());
				break;
			case ActionKind::Toggle:
				window.sidebar.collapsed = !window.sidebar.collapsed;
				resize(window);
				break;
			case ActionKind::Inspector:
				window.inspector = !window.inspector;
				window.dirty = true;
				break;
			case ActionKind::Restart:
				restart(window);
				break;
			case ActionKind::Split:
				splitTab(window);
				break;
			case ActionKind::Merge:
				mergeWindows(window.id());
				break;
			}
		}
	};
	auto finishDrag = [&](SDL_FPoint point) {
		if (!drag)
			return;
		const auto moving = *drag;
		drag.reset();
		if (drivePath.empty())
			SDL_CaptureMouse(false);
		if (!moving.moved)
			return;
		for (auto& window : windows)
			window->sidebarPress.reset();
		std::optional<std::reference_wrapper<Window>> dropWindow;
		for (const auto& candidate : windows) {
			int x = 0, y = 0;
			SDL_GetWindowPosition(candidate->window.get(), &x, &y);
			if (point.x < x || point.x >= x + candidate->windowW || point.y < y || point.y >= y + candidate->windowH)
				continue;
			if (!dropWindow || candidate->frontOrder > dropWindow->get().frontOrder)
				dropWindow = *candidate;
		}
		if (dropWindow) {
			auto& destination = dropWindow->get();
			int x = 0, y = 0;
			SDL_GetWindowPosition(destination.window.get(), &x, &y);
			const float localX = (point.x - x) * destination.toOutputX, localY = (point.y - y) * destination.toOutputY;
			if (localX >= destination.content.x)
				return;
			const float row = destination.sidebar.rowHeight(fonts), header = destination.sidebar.headerHeight(fonts);
			const auto insertion = static_cast<size_t>(
			    std::max(0.0f, std::floor((localY - header + destination.sidebar.scroll) / row + 0.5f)));
			moveTab(moving.window, moving.index, destination.id(), insertion, true);
		} else {
			const auto source = findWindow(moving.window);
			if (!source)
				return;
			selectTab(source->get(), moving.index);
			splitTab(source->get(), SDL_Point{static_cast<int>(point.x), static_cast<int>(point.y)});
		}
	};
	// Re-reads config.json and the theme: palette, wallpaper, window styling, every view. A
	// window's framebuffer alpha is fixed when it is created (SDL's swap chain has already shown
	// opaque frames), so a window created opaque says in its caption to restart.
	auto reloadConfig = [&] {
		theme = app::applyTheme(drivePath.empty());
		loadWallpaper();
		for (auto& window : windows) {
			app::styleWindow(*window->window, theme, window->translucent);
			window->engine->clear();
			for (auto& tab : window->tabs) {
				tab->applyTheme();
				tab->views.themeChanged();
			}
			window->notice = theme.transparent() && !window->translucent
			                   ? "Restart omp-ui to make this window translucent"
			                   : std::string{};
			window->dirty = true;
		}
	};
	auto pointerEvent = [&](Window& window, const SDL_Event& event, float outputX, float outputY) {
		float x = 0, y = 0, logicalX = 0, logicalY = 0;
		if (event.type == SDL_EVENT_MOUSE_MOTION) {
			logicalX = event.motion.x;
			logicalY = event.motion.y;
		} else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
			logicalX = event.button.x;
			logicalY = event.button.y;
		} else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
			logicalX = event.wheel.mouse_x;
			logicalY = event.wheel.mouse_y;
		} else
			return false;
		x = logicalX * outputX;
		y = logicalY * outputY;
		int windowX = 0, windowY = 0;
		SDL_GetWindowPosition(window.window.get(), &windowX, &windowY);
		const SDL_FPoint global{windowX + logicalX, windowY + logicalY};
		if (drag && event.type == SDL_EVENT_MOUSE_MOTION) {
			drag->point = global;
			if (std::abs(global.x - drag->start.x) + std::abs(global.y - drag->start.y) > 6)
				drag->moved = true;
			for (auto& item : windows)
				item->dirty = true;
			return true;
		}
		if (drag && event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
			const bool moved = drag->moved;
			finishDrag(global);
			if (moved)
				return true;
		}
		if (window.menu
		    && (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
		        || event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
			auto& menu = *window.menu;
			const float em = fonts.size(), row = fonts.lineHeight() + em * 0.6f;
			const bool inside = x >= menu.rect.x && x < menu.rect.x + menu.rect.w && y >= menu.rect.y + em * 0.3f
			                 && y < menu.rect.y + menu.rect.h - em * 0.3f;
			std::optional<size_t> at;
			if (inside)
				at = std::min(menu.entries.size() - 1, static_cast<size_t>((y - menu.rect.y - em * 0.3f) / row));
			if (at && menu.entries[*at].kind == SidebarMenu::Kind::Heading)
				at.reset();
			if (event.type == SDL_EVENT_MOUSE_MOTION) {
				window.dirty |= menu.hovered != at;
				menu.hovered = at;
				return true;
			}
			if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && at && event.button.button == SDL_BUTTON_LEFT) {
				const auto entry = menu.entries[*at];
				window.menu.reset();
				switch (entry.kind) {
				case SidebarMenu::Kind::PickFolder:
					chooseFolder(window);
					break;
				case SidebarMenu::Kind::NewSession:
					addTab(window, entry.folder, command, true);
					break;
				case SidebarMenu::Kind::ShowFolder:
					if (!SDL_OpenURL(fileUrl(text::utf8Path(entry.folder)).c_str()))
						SDL_Log("Cannot open %s: %s", entry.folder.c_str(), SDL_GetError());
					break;
				case SidebarMenu::Kind::CloseTab:
					if (entry.tab < window.tabs.size())
						closeTab(window, entry.tab);
					break;
				case SidebarMenu::Kind::ReloadConfig:
					reloadConfig();
					break;
				case SidebarMenu::Kind::Heading:
					break;
				}
			} else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !inside)
				window.menu.reset();
			window.dirty = true;
			return true;
		}
		if (const auto clicked = window.titleBar.pointer(event, x, y, window.dirty)) {
			switch (*clicked) {
			case app::TitleBar::Button::Minimize:
				SDL_MinimizeWindow(window.window.get());
				break;
			case app::TitleBar::Button::Maximize:
				if (SDL_GetWindowFlags(window.window.get()) & SDL_WINDOW_MAXIMIZED)
					SDL_RestoreWindow(window.window.get());
				else
					SDL_MaximizeWindow(window.window.get());
				break;
			case app::TitleBar::Button::Close:
				closeWindow(window.id()); // Destroys `window`.
				break;
			case app::TitleBar::Button::None:
				break;
			}
			return true;
		}
		if (!window.tabs.empty() && event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT
		    && window.tabs[window.active]->views.pointerCaptured()) {
			window.dirty |= window.tabs[window.active]->views.handle(event, outputX, outputY);
			return true;
		}
		if (event.type == SDL_EVENT_MOUSE_MOTION)
			window.dirty |= window.sidebar.motion(x, y, fonts, static_cast<float>(window.outputH), window.tabs.size());
		if (x < window.content.x) {
			if (event.type == SDL_EVENT_MOUSE_MOTION && !window.tabs.empty())
				window.dirty |= window.tabs[window.active]->views.handle(event, outputX, outputY);
			else if (event.type == SDL_EVENT_MOUSE_WHEEL)
				window.dirty |=
				    window.sidebar.wheel(event.wheel.y, fonts, static_cast<float>(window.outputH), window.tabs.size());
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
				window.sidebarPress = window.sidebar.hit(x, y, event.button.button, fonts,
				                                         static_cast<float>(window.outputH), window.tabs.size());
				if (window.sidebarPress->action == app::SidebarAction::Select
				    && event.button.button == SDL_BUTTON_LEFT) {
					drag = TabDrag{window.id(), window.sidebarPress->index, global, global};
					if (drivePath.empty())
						SDL_CaptureMouse(true);
				}
			} else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && window.sidebarPress) {
				const auto press = std::exchange(window.sidebarPress, std::nullopt);
				const auto hit = window.sidebar.hit(x, y, event.button.button, fonts,
				                                    static_cast<float>(window.outputH), window.tabs.size());
				if (hit.action != press->action || hit.index != press->index)
					return true;
				switch (hit.action) {
				case app::SidebarAction::Select:
					selectTab(window, hit.index);
					break;
				case app::SidebarAction::Close:
					closeTab(window, hit.index);
					break;
				case app::SidebarAction::New:
					newTab(window);
					break;
				case app::SidebarAction::NewMenu:
				case app::SidebarAction::HeaderMenu:
				case app::SidebarAction::TabMenu: {
					using Kind = SidebarMenu::Kind;
					SidebarMenu menu{{}, {}, std::nullopt};
					if (hit.action == app::SidebarAction::NewMenu) {
						menu.entries.push_back({Kind::PickFolder, "Open session in other folder…"});
						if (!recent.list().empty())
							menu.entries.push_back({Kind::Heading, "Recent"});
						for (const auto& folder : recent.list())
							menu.entries.push_back({Kind::NewSession, app::folderName(folder), folder, 0, true});
					} else if (hit.action == app::SidebarAction::HeaderMenu) {
						menu.entries.push_back(
						    {Kind::ShowFolder, "Open preferences folder", text::pathText(app::settingsFolder())});
						menu.entries.push_back({Kind::ReloadConfig, "Reload config"});
					} else if (hit.index < window.tabs.size()) {
						const auto& tab = *window.tabs[hit.index];
						menu.entries.push_back({Kind::NewSession, "New session in this folder", tab.cwd});
						menu.entries.push_back({Kind::ShowFolder, "Show folder", tab.cwd});
						menu.entries.push_back({Kind::CloseTab, "Close tab", {}, hit.index});
					}
					if (menu.entries.empty())
						break;
					const float em = fonts.size(), row = fonts.lineHeight() + em * 0.6f;
					const float w = std::min(em * (hit.action == app::SidebarAction::NewMenu ? 30 : 16),
					                         static_cast<float>(window.outputW) - x);
					const float h = row * static_cast<float>(menu.entries.size()) + em * 0.6f;
					menu.rect = {x, std::max(0.0f, std::min(y, static_cast<float>(window.outputH) - h)), w, h};
					window.menu = std::move(menu);
					window.dirty = true;
					break;
				}
				case app::SidebarAction::Toggle:
					window.sidebar.collapsed = !window.sidebar.collapsed;
					resize(window);
					break;
				default:
					break;
				}
			}
			return true;
		}
		window.sidebarPress.reset();
		if (!window.tabs.empty() && window.tabs[window.active]->exited && event.type == SDL_EVENT_MOUSE_BUTTON_UP
		    && event.button.button == SDL_BUTTON_LEFT && x >= window.restart.x
		    && x < window.restart.x + window.restart.w && y >= window.restart.y
		    && y < window.restart.y + window.restart.h) {
			restart(window);
			return true;
		}
		if (window.tabs.empty() && event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT
		    && x >= window.openWorkspace.x && x < window.openWorkspace.x + window.openWorkspace.w
		    && y >= window.openWorkspace.y && y < window.openWorkspace.y + window.openWorkspace.h) {
			chooseFolder(window);
			return true;
		}
		return false;
	};
	auto label = [&](Window& window, std::string value, float x, float y, float width, text::RunStyle style = {}) {
		text::Runs runs{{std::move(value), std::move(style)}};
		const auto layout =
		    window.engine->layout(runs, {.width = std::max(1.0f, width), .wrap = text::Wrap::None, .maxLines = 1});
		window.engine->draw(runs, layout, x, y);
	};
	// The theme's picture over the canvas, behind the transcript.
	auto paintWallpaper = [&](Window& window) {
		if (!wallpaper)
			return;
		const SDL_FRect area{window.content.x, 0, window.content.w, static_cast<float>(window.outputH)};
		const float width = wallpaper->width() * scale, height = wallpaper->height() * scale;
		gfx::ScopedClip clip(*window.renderer, area);
		const auto centred = [&](float w, float h) {
			return SDL_FRect{area.x + (area.w - w) * 0.5f, area.y + (area.h - h) * 0.5f, w, h};
		};
		switch (theme.fit) {
		case app::Theme::Fit::Cover:
		case app::Theme::Fit::Contain: {
			const float ratioW = area.w / wallpaper->width(), ratioH = area.h / wallpaper->height();
			const float ratio =
			    theme.fit == app::Theme::Fit::Cover ? std::max(ratioW, ratioH) : std::min(ratioW, ratioH);
			window.engine->drawImage(*wallpaper, centred(wallpaper->width() * ratio, wallpaper->height() * ratio),
			                         theme.imageOpacity);
			break;
		}
		case app::Theme::Fit::Center:
			window.engine->drawImage(*wallpaper, centred(width, height), theme.imageOpacity);
			break;
		case app::Theme::Fit::Tile:
			for (float y = area.y; y < area.y + area.h; y += height)
				for (float x = area.x; x < area.x + area.w; x += width)
					window.engine->drawImage(*wallpaper, {x, y, width, height}, theme.imageOpacity);
			break;
		}
	};
	// Draws everything into the current target. `partial`: only inside the renderer clip (an
	// earlier frame's animation areas), nothing else having changed. Returns where this frame's
	// animations are, or nullopt when unknown (the next animation frame then repaints fully).
	auto paintScene = [&](Window& window, view::Views::Paint mode) -> std::optional<std::vector<SDL_FRect>> {
		window.animating = false;
		std::optional<std::vector<SDL_FRect>> animated = std::vector<SDL_FRect>{};
		const float margin = fonts.size() * 1.2f, pitch = fonts.lineHeight();
		// Premultiplied, as DWM composes a translucent window's framebuffer; a window created
		// opaque stays opaque even if a reloaded theme is translucent. A full paint clears (GPUs
		// fast-clear); a partial one replaces just its clip with a fill.
		const auto canvas = text::premultiplied(window.translucent ? text::palette::canvas
		                                                           : text::withAlpha(text::palette::canvas, 255));
		SDL_SetRenderDrawColor(window.renderer.get(), canvas.r, canvas.g, canvas.b, canvas.a);
		if (mode == view::Views::Paint::Full)
			SDL_RenderClear(window.renderer.get());
		else {
			const SDL_FRect output{0, 0, static_cast<float>(window.outputW), static_cast<float>(window.outputH)};
			SDL_SetRenderDrawBlendMode(window.renderer.get(), SDL_BLENDMODE_NONE);
			SDL_RenderFillRect(window.renderer.get(), &output);
			SDL_SetRenderDrawBlendMode(window.renderer.get(), SDL_BLENDMODE_BLEND);
		}
		paintWallpaper(window);
		if (!window.tabs.empty()) {
			auto& tab = *window.tabs[window.active];
			auto& host = tab.host;
			const float inspectorHeight =
			    window.inspector ? std::min(window.outputH * 0.48f, pitch * 12 + margin * 2) : 0;
			const float exitHeight = tab.exited ? fonts.size() * 3.5f : 0;
			const SDL_FRect viewport{window.content.x, window.content.y, window.content.w,
			                         std::max(1.0f, window.content.h - inspectorHeight - exitHeight)};
			if (!host.session.surfaces().empty()) {
				window.animating = tab.views.paint(viewport, mode);
				animated = tab.views.animatedRegions();
				window.input->setCaret(tab.views.focusedCaret(), window.toOutputX, window.toOutputY);
			} else {
				const auto status = tab.exited                    ? std::format("exited with {}", tab.exitCode)
				                  : !host.session.helloAnswered() ? "waiting for hello"
				                                                  : "waiting for surface";
				label(window, status, window.content.x + margin, window.content.y + margin * 0.5f,
				      window.content.w - 2 * margin, {.color = text::palette::muted});
				if (!host.rowsText.empty()) {
					const auto begin = host.rowsText.size() > 8192 ? host.rowsText.size() - 8192 : 0;
					text::Runs tail{
					    {host.rowsText.substr(begin), {.font = text::FontId::Mono, .color = text::palette::muted}}};
					const auto laid =
					    window.engine->layout(tail, {.width = std::max(1.0f, window.content.w - 2 * margin)});
					gfx::ClipStack clip(*window.renderer);
					clip.push(viewport);
					window.engine->draw(tail, laid, window.content.x + margin,
					                    window.content.y + pitch + margin * 1.6f);
					clip.pop();
				}
				window.input->setCaret(std::nullopt, window.toOutputX, window.toOutputY);
			}
			window.restart = {};
			if (tab.exited) {
				const float y = window.content.y + window.content.h - inspectorHeight - exitHeight;
				gfx::fill(*window.renderer, {window.content.x, y, window.content.w, exitHeight},
				          text::palette::surface);
				window.restart = {window.content.x + window.content.w - fonts.size() * 9, y + fonts.size() * 0.6f,
				                  fonts.size() * 8, fonts.size() * 2.3f};
				gfx::rounded(*window.renderer, window.restart, fonts.size() * 0.4f, text::palette::surface2);
				label(window, std::format("Process exited · code {}", tab.exitCode), window.content.x + margin,
				      y + (exitHeight - pitch) * 0.5f, window.content.w - fonts.size() * 11,
				      {.color = text::palette::muted});
				label(window, "Restart ↵", window.restart.x + fonts.size() * 0.65f,
				      window.restart.y + (window.restart.h - pitch) * 0.5f, window.restart.w - fonts.size(),
				      {.color = text::palette::accent});
			}
			if (window.inspector) {
				const float panelY = static_cast<float>(window.outputH) - inspectorHeight;
				gfx::fill(*window.renderer, {window.content.x, panelY, window.content.w, inspectorHeight},
				          text::palette::surface2);
				const float x = window.content.x + margin, width = window.content.w - margin * 2;
				label(window,
				      std::format("TSP · tab {} · pid {} · {} · scale {:.2f} · {} cols · frames {} ops {} rejected {}",
				                  window.active + 1, tab.pty.pid(), tab.pty.backend(), scale, host.session.cols,
				                  host.session.framesApplied, host.session.opsApplied, host.session.opErrors),
				      x, panelY + margin * 0.4f, width, {.font = text::FontId::Mono, .color = text::palette::info});
				label(window, tab.views.dirtySummary(), x, panelY + margin * 0.4f + pitch, width,
				      {.font = text::FontId::Mono, .color = text::palette::warning});
				const size_t count = std::max(1, static_cast<int>((inspectorHeight - margin - pitch * 2) / pitch));
				const size_t first = host.session.log.size() > count ? host.session.log.size() - count : 0;
				float lineY = panelY + margin * 0.4f + pitch * 2;
				for (size_t i = first; i < host.session.log.size(); ++i) {
					const auto& entry = host.session.log[i];
					label(window, std::format("{} {} {}", entry.out ? "→" : "←", entry.verb, entry.text), x, lineY,
					      width,
					      {.font = text::FontId::Mono,
					       .color = entry.out ? text::palette::success : text::palette::muted});
					lineY += pitch;
				}
			}
		} else {
			// No session: one centred action instead of a dialog popping up at launch.
			const std::string caption = gfx::iconGlyph("folder") + "  Open workspace";
			const float em = fonts.size();
			const float w = fonts.width(text::FontId::Bold, caption) + em * 2.5f, h = pitch + em * 1.2f;
			window.openWorkspace = {window.content.x + (window.content.w - w) * 0.5f,
			                        window.content.y + (window.content.h - h) * 0.5f, w, h};
			gfx::rounded(*window.renderer, window.openWorkspace, h * 0.5f, text::palette::surface2);
			gfx::roundedBorder(*window.renderer, window.openWorkspace, h * 0.5f, text::palette::border);
			label(window, caption, window.openWorkspace.x + em * 1.25f, window.openWorkspace.y + (h - pitch) * 0.5f,
			      w - em * 2.5f, {.font = text::FontId::Bold, .color = text::palette::fg});
		}
		// The caption: the active tab's title, then the window buttons.
		{
			const auto area = window.titleBar.titleArea();
			const float em = fonts.size();
			if (!window.tabs.empty() && area.w > em * 4)
				label(window, window.tabs[window.active]->title, area.x + em, area.y + (area.h - pitch) * 0.5f,
				      area.w - em * 2, {.color = text::palette::muted});
			if (!window.notice.empty()) {
				const float width = fonts.width(text::FontId::Ui, window.notice);
				label(window, window.notice, area.x + area.w - width - em, area.y + (area.h - pitch) * 0.5f, width + 1,
				      {.color = text::palette::warning});
			}
			const bool maximized = (SDL_GetWindowFlags(window.window.get()) & SDL_WINDOW_MAXIMIZED) != 0;
			window.titleBar.paint(*window.renderer, maximized, em);
		}
		const bool sidebarAnimating =
		    window.sidebar.paint(*window.renderer, *window.engine, fonts, window.tabs, window.active,
		                         static_cast<float>(window.outputH), window.translucent);
		window.animating |= sidebarAnimating;
		if (animated && sidebarAnimating)
			animated->push_back({0, 0, window.sidebar.width(fonts), static_cast<float>(window.outputH)});
		window.sidebar.paintTooltip(*window.renderer, *window.engine, fonts, window.tabs,
		                            static_cast<float>(window.outputW), static_cast<float>(window.outputH));
		if (window.dimmedAt) {
			// OLED care: an idle window darkens what stays put for hours (sidebar, title bar, the
			// prompt dock) as a vignette around the transcript, which is left as it is. Fades in
			// over a second and a half; input brings everything back at once.
			const float fade = std::min(1.0f, static_cast<float>(SDL_GetTicks() - window.dimmedAt) / 1500);
			const SDL_FRect output{0, 0, static_cast<float>(window.outputW), static_cast<float>(window.outputH)};
			float bottom = window.content.y + window.content.h;
			if (!window.tabs.empty()) {
				auto& tab = *window.tabs[window.active];
				for (const auto& surface : tab.host.session.surfaces())
					if (surface->open && surface->mode != "screen")
						if (const auto dock = tab.views.nodeRect(surface->id, "dock"); dock && dock->h > 0)
							bottom = std::min(bottom, dock->y);
			}
			const SDL_FRect transcript{window.content.x, window.content.y, window.content.w,
			                           std::max(0.0f, bottom - window.content.y)};
			gfx::shadeAround(*window.renderer, output, transcript, fonts.size() * 2.5f,
			                 static_cast<Uint8>(std::lround(fade * fade * 170)));
			if (fade < 1) {
				window.animating = true;
				animated.reset(); // The whole window changes until the fade ends.
			}
		}
		if (window.menu) {
			const auto& menu = *window.menu;
			const float em = fonts.size(), row = pitch + em * 0.6f;
			gfx::rounded(*window.renderer, {menu.rect.x, menu.rect.y + em * 0.2f, menu.rect.w, menu.rect.h}, em * 0.5f,
			             text::rgb(0, 70));
			gfx::rounded(*window.renderer, menu.rect, em * 0.5f, text::palette::surface2);
			gfx::roundedBorder(*window.renderer, menu.rect, em * 0.5f, text::palette::border);
			for (size_t index = 0; index < menu.entries.size(); ++index) {
				const auto& entry = menu.entries[index];
				const SDL_FRect item{menu.rect.x + em * 0.3f, menu.rect.y + em * 0.3f + row * static_cast<float>(index),
				                     menu.rect.w - em * 0.6f, row};
				const float textY = item.y + (row - pitch) * 0.5f;
				if (menu.hovered == index)
					gfx::rounded(*window.renderer, item, em * 0.35f, text::palette::border);
				if (entry.kind == SidebarMenu::Kind::Heading) {
					label(window, entry.label, item.x + em * 0.7f, textY, item.w - em * 1.4f,
					      {.font = text::FontId::Bold, .color = text::palette::dim});
				} else if (entry.showPath) {
					// A recent folder: its name, then its full path dimmed and cut in the middle.
					const float nameWidth = std::min(fonts.width(text::FontId::Ui, entry.label), item.w * 0.45f);
					label(window, entry.label, item.x + em * 0.7f, textY, nameWidth + 1);
					text::Runs path{{entry.folder, {.color = text::palette::dim}}};
					const auto layout =
					    window.engine->layout(path, {.width = std::max(1.0f, item.w - nameWidth - em * 2.4f),
					                                 .wrap = text::Wrap::None,
					                                 .truncate = text::Truncate::Middle,
					                                 .maxLines = 1});
					window.engine->draw(path, layout, item.x + item.w - em * 0.7f - layout.width, textY);
				} else
					label(window, entry.label, item.x + em * 0.7f, textY, item.w - em * 1.4f);
			}
		}
		if (drag && drag->moved) {
			int x = 0, y = 0;
			SDL_GetWindowPosition(window.window.get(), &x, &y);
			const float localX = (drag->point.x - x) * window.toOutputX,
			            localY = (drag->point.y - y) * window.toOutputY;
			if (localX >= 0 && localX < window.content.x && localY >= 0 && localY < window.outputH) {
				const float row = window.sidebar.rowHeight(fonts), header = window.sidebar.headerHeight(fonts);
				const float index = std::clamp(std::floor((localY - header + window.sidebar.scroll) / row + 0.5f), 0.0f,
				                               static_cast<float>(window.tabs.size()));
				const float at = header + index * row - window.sidebar.scroll;
				gfx::line(*window.renderer, {fonts.size() * 0.5f, at}, {window.content.x - fonts.size() * 0.5f, at},
				          text::palette::accent, 2 * scale);
			}
		}
		return animated;
	};
	// Shows the window. While something animates (spinners, shimmer, timers) the frame is kept
	// in a window-sized texture and later frames repaint only the animated areas, instead of the
	// whole transcript 30 times a second. Without animations frames draw straight to the screen
	// (no extra full-window copy) and the texture is released, so idle windows hold no frame VRAM.
	auto paint = [&](Window& window) {
		auto& renderer = *window.renderer;
		// Animating, or timed leaves (spinners, clocks) whose areas the last paint recorded.
		if (!window.animating && !(window.damage && !window.damage->empty())) {
			window.frame.reset();
			window.damage.reset();
		} else {
			float frameW = 0, frameH = 0;
			if (window.frame)
				SDL_GetTextureSize(window.frame.get(), &frameW, &frameH);
			if (!window.frame || static_cast<int>(frameW) != window.outputW
			    || static_cast<int>(frameH) != window.outputH) {
				window.frame.reset(SDL_CreateTexture(&renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
				                                     window.outputW, window.outputH));
				if (window.frame)
					SDL_SetTextureBlendMode(window.frame.get(), SDL_BLENDMODE_NONE);
				window.damage.reset(); // A new texture holds no previous frame.
			}
		}
		const bool partial = window.frame && !window.dirty && window.damage && !window.damage->empty();
		if (window.frame)
			SDL_SetRenderTarget(&renderer, window.frame.get());
		if (partial) {
			// Views accumulate animation areas and wake-ups over a frame's regions, so the last
			// region's result holds them all; any unknown area forces a full paint next.
			std::optional<std::vector<SDL_FRect>> next;
			bool animating = false, known = true, first = true;
			for (const auto& region : damageRegions(*window.damage)) {
				const SDL_Rect clip{static_cast<int>(std::floor(region.x)), static_cast<int>(std::floor(region.y)),
				                    static_cast<int>(std::ceil(region.x + region.w) - std::floor(region.x)),
				                    static_cast<int>(std::ceil(region.y + region.h) - std::floor(region.y))};
				SDL_SetRenderClipRect(&renderer, &clip);
				next = paintScene(window, first ? view::Views::Paint::Partial : view::Views::Paint::PartialMore);
				first = false;
				animating |= window.animating;
				known &= next.has_value();
			}
			SDL_SetRenderClipRect(&renderer, nullptr);
			window.animating = animating;
			window.damage = known ? std::move(next) : std::nullopt;
		} else
			window.damage = paintScene(window, view::Views::Paint::Full);
		// Animating without a known area: repaint fully next time.
		if (window.animating && window.damage && window.damage->empty())
			window.damage.reset();
		if (window.frame) {
			SDL_SetRenderTarget(&renderer, nullptr);
			SDL_RenderTexture(&renderer, window.frame.get(), nullptr, nullptr);
		}
		if (drive && window.id() == targetWindow)
			drive->beforePresent();
		if (window.composition.attached())
			window.composition.present(*window.renderer);
		else
			SDL_RenderPresent(window.renderer.get());
		window.lastPaint = SDL_GetTicks();
		++window.paintCount;
		if (window.switchStarted) {
			window.lastSwitchMs =
			    (SDL_GetPerformanceCounter() - *window.switchStarted) * 1000.0 / SDL_GetPerformanceFrequency();
			SDL_Log("tab-switch window %u tab %zu: %.3f ms, one layout/paint", window.id(), window.active + 1,
			        window.lastSwitchMs);
			window.switchStarted.reset();
		}
		window.dirty = false;
		window.due = false;
		if (!window.tabs.empty()) {
			auto& tab = *window.tabs[window.active];
			const auto presentedLog = tab.logGeneration;
			tab.host.session.flushAcks();
			window.dirty = window.inspector && tab.logGeneration != presentedLog;
		}
	};

	if (!workspace.empty())
		addTab(*windows.front(), workspace, command, true);
	if (!drivePath.empty()) {
		auto& window = *windows.front();
		auto& tab = *window.tabs[window.active];
		drive = std::make_unique<app::Drive>(
		    drivePath, *window.window, *window.renderer, tab.host.session, tab.views,
		    [&](float value) {
			    forcedScale = value;
			    setScale(value);
		    },
		    [&] {
			    if (const auto found = findWindow(targetWindow)) {
				    found->get().inspector = !found->get().inspector;
				    found->get().dirty = true;
			    }
		    },
		    *window.input);
		drive->setEventHandler([&](const SDL_Event& event) {
			const auto found = findWindow(targetWindow);
			if (found && event.type == SDL_EVENT_DROP_FILE) {
				dropped(found->get(), event.drop.data);
				return true;
			}
			return found ? pointerEvent(found->get(), event, 1, 1) : true;
		});
		drive->setTabAction([&](std::string_view action, std::string_view argument) -> std::string {
			const auto found = findWindow(targetWindow);
			if (!found)
				return "No target window";
			auto& current = found->get();
			if (action == "idle") { // As if `dimAfter` ran out: the idle fade starts now.
				current.dimmedAt = SDL_GetTicks();
				current.dirty = true;
				return {};
			}
			if (action == "window") {
				std::istringstream value{std::string(argument)};
				size_t index = 0;
				if (!(value >> index) || index == 0 || index > windows.size())
					return "window requires an existing 1-based window index";
				target(windows[index - 1]->id());
				return {};
			}
			if (action == "newwindow") {
				splitTab(current);
				return {};
			}
			if (action == "merge") {
				mergeWindows(current.id());
				return {};
			}
			if (action == "tabdrag") {
				std::istringstream values{std::string(argument)};
				size_t from = 0, index = 0, insertion = 0;
				std::string destination;
				if (!(values >> from >> index >> destination >> insertion) || from == 0 || from > windows.size()
				    || index == 0 || index > windows[from - 1]->tabs.size() || insertion == 0)
					return "tabdrag requires source-window tab destination-window|new insertion (1-based)";
				auto& source = *windows[from - 1];
				int sourceX = 0, sourceY = 0;
				SDL_GetWindowPosition(source.window.get(), &sourceX, &sourceY);
				const SDL_FPoint start{sourceX + source.sidebar.width(fonts) / (2 * source.toOutputX),
				                       sourceY
				                           + (source.sidebar.headerHeight(fonts)
				                              + (static_cast<float>(index) - 0.5f) * source.sidebar.rowHeight(fonts)
				                              - source.sidebar.scroll)
				                                 / source.toOutputY};
				SDL_FPoint end{};
				if (destination == "new") {
					float right = 0, bottom = 0;
					for (const auto& item : windows) {
						int x = 0, y = 0;
						SDL_GetWindowPosition(item->window.get(), &x, &y);
						right = std::max(right, static_cast<float>(x + item->windowW));
						bottom = std::max(bottom, static_cast<float>(y + item->windowH));
					}
					end = {right + 50, bottom + 50};
				} else {
					std::istringstream value(destination);
					size_t to = 0;
					if (!(value >> to) || to == 0 || to > windows.size())
						return "tabdrag destination window does not exist";
					auto& destinationWindow = *windows[to - 1];
					int x = 0, y = 0;
					SDL_GetWindowPosition(destinationWindow.window.get(), &x, &y);
					end = {x + fonts.size() / destinationWindow.toOutputX,
					       y
					           + (destinationWindow.sidebar.headerHeight(fonts)
					              + (static_cast<float>(insertion) - 1) * destinationWindow.sidebar.rowHeight(fonts)
					              - destinationWindow.sidebar.scroll)
					                 / destinationWindow.toOutputY};
				}
				SDL_Event down{};
				down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
				down.button.button = SDL_BUTTON_LEFT;
				down.button.x = start.x - sourceX;
				down.button.y = start.y - sourceY;
				pointerEvent(source, down, source.toOutputX, source.toOutputY);
				SDL_Event motion{};
				motion.type = SDL_EVENT_MOUSE_MOTION;
				motion.motion.x = end.x - sourceX;
				motion.motion.y = end.y - sourceY;
				pointerEvent(source, motion, source.toOutputX, source.toOutputY);
				SDL_Event up{};
				up.type = SDL_EVENT_MOUSE_BUTTON_UP;
				up.button.button = SDL_BUTTON_LEFT;
				up.button.x = motion.motion.x;
				up.button.y = motion.motion.y;
				pointerEvent(source, up, source.toOutputX, source.toOutputY);
				return {};
			}
			if (action == "tabs") {
				json all = json::array(), selected = json::array();
				for (const auto& window : windows) {
					json entries = json::array();
					for (const auto& tab : window->tabs) {
						json log = json::array();
						for (const auto& entry : tab->host.session.log)
							log.push_back({{"out", entry.out}, {"verb", entry.verb}, {"text", entry.text}});
						entries.push_back({{"cwd", tab->cwd},
						                   {"title", tab->title},
						                   {"pid", tab->pty.pid()},
						                   {"cols", tab->host.session.cols},
						                   {"rows", tab->host.rows},
						                   {"running", tab->running},
						                   {"attention", tab->attention},
						                   {"exited", tab->exited},
						                   {"exitCode", tab->exitCode},
						                   {"visible", tab->visible},
						                   {"frames", tab->host.session.framesApplied},
						                   {"log", std::move(log)}});
					}
					if (window->id() == targetWindow)
						selected = entries;
					all.push_back({{"id", window->id()},
					               {"active", window->active},
					               {"width", window->outputW},
					               {"height", window->outputH},
					               {"tabs", std::move(entries)}});
				}
				std::ofstream file(text::utf8Path(argument), std::ios::binary);
				file << json{{"target", targetWindow},
				             {"active", current.active},
				             {"collapsed", current.sidebar.collapsed},
				             {"switchMs", current.lastSwitchMs},
				             {"switchCount", current.switchCount},
				             {"paints", current.paintCount},
				             {"tabs", std::move(selected)},
				             {"windows", std::move(all)}}
				            .dump(2)
				     << '\n';
				return file ? std::string{} : "Cannot write tabs snapshot";
			}
			std::string cwd(argument);
			std::vector<std::string> argv = command;
			bool activate = action != "tabbg";
			if (action == "tabcmd") {
				try {
					const auto spec = json::parse(argument);
					cwd = spec.value("cwd", current.tabs[current.active]->cwd);
					argv = spec.at("argv").get<std::vector<std::string>>();
					activate = spec.value("active", false);
				} catch (const json::exception& exception) {
					return exception.what();
				}
			}
			std::error_code ec;
			if (argv.empty() || !std::filesystem::is_directory(text::utf8Path(cwd), ec))
				return "tab requires an existing folder and nonempty command";
			addTab(current, cwd, std::move(argv), activate);
			return {};
		});
		if (!drive->error().empty())
			return fail(drive->error());
	}
	// Live resizing runs inside the system's modal size loop, which starves this loop; SDL keeps
	// sending expose events to event watchers there, so the window is redrawn at its new size.
	std::function<void(const SDL_Event&)> liveResize = [&](const SDL_Event& event) {
		if (event.type != SDL_EVENT_WINDOW_EXPOSED)
			return;
		const auto found = findWindow(event.window.windowID);
		if (!found)
			return;
		auto& window = found->get();
		int width = 0, height = 0;
		SDL_GetWindowSizeInPixels(window.window.get(), &width, &height);
		if (width == window.outputW && height == window.outputH)
			return;
		resize(window);
		paint(window);
	};
	const auto watch = [](void* data, SDL_Event* event) {
		(*static_cast<std::function<void(const SDL_Event&)>*>(data))(*event);
		return true;
	};
	SDL_AddEventWatch(watch, &liveResize);
	for (auto& window : windows)
		paint(*window);
	while (running) {
		if (drive) {
			const bool changed = drive->tick(running);
			if (const auto found = findWindow(targetWindow))
				found->get().dirty |= changed;
		}
		processActions();
		if (!running)
			break;
		auto earlier = [](int a, int b) {
			return a < 0 ? b : b < 0 ? a : std::min(a, b);
		};
		int delay = -1;
		for (auto& window : windows) {
			if (theme.dimAfter > 0 && !window->dimmedAt) {
				const auto dimAt = window->lastInput + static_cast<Uint64>(theme.dimAfter * 1000);
				const auto now = SDL_GetTicks();
				if (now >= dimAt) {
					window->dimmedAt = now;
					window->dirty = true;
				} else
					delay = earlier(delay, static_cast<int>(std::min<Uint64>(dimAt - now, 0x7fffffff)));
			}
			if (!window->tabs.empty()) {
				window->input->setEnabled(!window->tabs[window->active]->exited);
				window->wake(window->tabs[window->active]->views.tick());
				delay = earlier(delay, window->tabs[window->active]->views.wakeDelay());
			}
			window->tooltipPending = window->sidebar.wakeDelay() >= 0;
			delay = earlier(delay, window->sidebar.wakeDelay());
			if (window->dirty || window->due)
				delay = 0;
			else if (window->animating) // Animation frames at ~30 fps.
				delay = earlier(delay, static_cast<int>(std::max<Sint64>(0, static_cast<Sint64>(window->lastPaint + 33)
				                                                                - SDL_GetTicks())));
		}
		if (drive)
			delay = earlier(delay, drive->wakeDelay());
		SDL_Event event{};
		if (SDL_WaitEventTimeout(&event, delay))
			do {
				if (event.type == SDL_EVENT_QUIT) {
					running = false;
					break;
				}
				const auto found = findWindow(eventWindow(event));
				if (!found)
					continue;
				auto& window = found->get();
				switch (event.type) {
				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_TEXT_INPUT:
				case SDL_EVENT_MOUSE_MOTION:
				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_WHEEL:
				case SDL_EVENT_DROP_FILE:
					window.lastInput = SDL_GetTicks();
					if (window.dimmedAt) {
						window.dimmedAt = 0;
						window.dirty = true;
					}
					break;
				default:
					break;
				}
				if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
					window.titleBar.pointer(event, -1, -1, window.dirty);
				if (event.type == SDL_EVENT_KEY_DOWN && window.menu && event.key.key == SDLK_ESCAPE) {
					window.menu.reset();
					window.dirty = true;
					continue;
				}
				if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
					closeWindow(window.id());
					if (!running)
						break;
					continue;
				}
				if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
					dropped(window, event.drop.data);
					continue;
				}
				if (pointerEvent(window, event, window.toOutputX, window.toOutputY)) {
					if (!running)
						break;
					continue;
				}
				if (!window.tabs.empty()) {
					window.dirty |= window.tabs[window.active]->views.handle(event, window.toOutputX, window.toOutputY);
					if (window.input->handle(event)) {
						processActions(); // A switch must precede the next queued key/text event.
						if (!running)
							break;
						continue;
					}
				}
				switch (event.type) {
				case SDL_EVENT_WINDOW_RESIZED:
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
				case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
					if (!forcedScale)
						setScale(SDL_GetWindowDisplayScale(window.window.get()));
					resize(window);
					break;
				case SDL_EVENT_WINDOW_EXPOSED:
					window.dirty = true;
					break;
				case SDL_EVENT_WINDOW_FOCUS_GAINED:
					window.frontOrder = ++frontOrder;
					target(window.id());
					break;
				}
			} while (running && SDL_PollEvent(&event));
		else
			for (auto& window : windows)
				if (window->tooltipPending && window->sidebar.wakeDelay() < 0)
					window->dirty = true; // The sidebar tooltip's delay ran out.
		processActions();
		if (!running)
			break;
		std::vector<FolderReply> replies;
		{
			std::scoped_lock lock(folders.mutex);
			replies.swap(folders.replies);
		}
		for (auto& reply : replies) {
			const auto found = findWindow(reply.window);
			if (!found)
				continue;
			auto& window = found->get();
			window.choosingFolder = false;
			if (reply.path)
				addTab(window, *reply.path, command, true);
		}
		if (!running)
			break;
		for (auto& window : windows) {
			for (size_t i = 0; i < window->tabs.size(); ++i) {
				auto& tab = *window->tabs[i];
				const auto titleGeneration = tab.titleGeneration;
				const bool attention = tab.attention, exited = tab.exited;
				const int working = tab.running;
				const bool changed = tab.drain();
				window->dirty |= (i == window->active && changed) || attention != tab.attention || exited != tab.exited
				              || working != tab.running || titleGeneration != tab.titleGeneration;
				if (i == window->active && titleGeneration != tab.titleGeneration)
					SDL_SetWindowTitle(window->window.get(), (tab.title + " — omp-ui").c_str());
				tabMetrics(*window, tab);
			}
			if (!window->tabs.empty()) {
				window->wake(window->tabs[window->active]->views.tick());
			}
			if (window->dirty || window->due || (window->animating && SDL_GetTicks() >= window->lastPaint + 33))
				paint(*window);
		}
	}
	SDL_RemoveEventWatch(watch, &liveResize);
	windows.clear();
	return drive && !drive->error().empty() ? 1 : 0;
}
