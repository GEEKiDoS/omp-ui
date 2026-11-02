#pragma once

#include "core/host.h"
#include "core/pty.h"
#include "gfx/tween.h"
#include "view/view_tree.h"

#include <mutex>
#include <optional>
#include <span>

namespace app {

struct Inbox {
	std::mutex mutex;
	std::vector<std::string> bytes;
	std::optional<int> exited;
	Uint32 event;
	bool pending = false;

	void wake() const;
	void append(std::string_view value);
	void exit(int code);
	bool drain(TerminalHost& host, std::optional<int>& exitCode);
};

struct Tab {
	Tab(std::string cwd, std::string title, std::vector<std::string> command, Uint32 wakeEvent, SDL_Renderer& renderer,
	    text::Engine& engine, const text::Fonts& fonts);
	~Tab();
	bool start(int cols, int rows, std::string& error);
	bool drain();
	void setVisible(bool value);
	// Takes the current palette for omp (hello's `dark`, OSC 10/11 answers).
	void applyTheme();

	Pty pty;
	TerminalHost host;
	view::Views views;
	Inbox inbox;
	std::string cwd, title;
	std::vector<std::string> command;
	int running = 0;
	bool attention = false, exited = false, visible = false;
	int exitCode = 0;
	uint64_t logGeneration = 0;
	// omp's last bell or notification message since the app last looked (empty message: a bell).
	std::optional<std::string> notification;
	uint64_t titleGeneration = 0;
};

std::string folderName(std::string_view cwd);
std::string currentFolder();

// Workspace folders sessions were opened in, newest first, offered by the new-tab menu.
// Kept in the app's pref folder as recent.json; memory only when `persist` is false (test runs).
class RecentFolders {
public:
	explicit RecentFolders(bool persist);
	void add(const std::string& folder);
	const std::vector<std::string>& list() const { return folders_; }

private:
	std::string file_;
	std::vector<std::string> folders_;
};

// Left/middle-button actions, plus the right-click menus of the "+" row, the header and a tab row.
enum class SidebarAction { None, Select, Close, New, NewMenu, HeaderMenu, TabMenu, Toggle };
struct SidebarHit {
	SidebarAction action = SidebarAction::None;
	size_t index = 0;
};
struct Sidebar {
	Sidebar() { expansion.snap(1); }
	bool collapsed = false;
	std::optional<size_t> hovered;
	float pointerX = -1, pointerY = -1, scroll = 0;
	Uint64 hoverSince = 0;
	gfx::Tween expansion;
	void toggle();
	bool transitioning(Uint64 now = SDL_GetTicks()) const { return expansion.active(now); }
	float width(const text::Fonts& fonts, Uint64 now = SDL_GetTicks()) const;
	float targetWidth(const text::Fonts& fonts) const;
	float rowHeight(const text::Fonts& fonts) const;
	float headerHeight(const text::Fonts& fonts) const;
	float plusY(const text::Fonts& fonts, float height, size_t count) const;
	SidebarHit hit(float x, float y, int button, const text::Fonts& fonts, float height, size_t count,
	               float visibleWidth, bool moving) const;
	bool motion(float x, float y, const text::Fonts& fonts, float height, size_t count, float visibleWidth,
	            bool moving);
	bool wheel(float delta, const text::Fonts& fonts, float height, size_t count, bool moving);
	void reveal(size_t index, const text::Fonts& fonts, float height, size_t count);
	// `translucent`: the window has an alpha framebuffer, so the sidebar may be see-through.
	bool paint(SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts,
	           std::span<const std::unique_ptr<Tab>> tabs, size_t active, float height, float visibleWidth,
	           bool translucent) const;
	void paintTooltip(SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts,
	                  std::span<const std::unique_ptr<Tab>> tabs, float outputWidth, float outputHeight,
	                  bool moving) const;
	int wakeDelay() const;
};

} // namespace app
