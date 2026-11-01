#pragma once

#include <SDL3/SDL.h>

#include "host.h"
#include "pty.h"

#include <array>
#ifdef _WIN32
#include <deque>
#endif
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace app {

class Input {
public:
	Input(SDL_Window& window, TerminalHost& host, Pty& pty);
	~Input();
	Input(const Input&) = delete;
	Input& operator=(const Input&) = delete;

	bool handle(const SDL_Event& event);
	void setEnabled(bool enabled) { enabled_ = enabled; }
	void setTarget(TerminalHost& host, Pty& pty) {
		host_ = host;
		pty_ = pty;
	}
	// Called before terminal encoding; true consumes the shortcut and its text.
	std::function<bool(const SDL_KeyboardEvent&)> onShortcut;
	// Editing shortcuts the window wants right now (copy/cut with a selection, select all):
	// claimed keys go to onShortcut instead of the program.
	std::function<bool(SDL_Keycode, SDL_Keymod)> claimKey;
	bool claimed(SDL_Keycode key, SDL_Keymod mod) const { return claimKey && claimKey(key, mod); }
	// Offered every key press (with its text, when it types) before it is encoded for the program;
	// true consumes it. Lets a terminal-side editor selection be replaced or deleted.
	std::function<bool(SDL_Keycode, SDL_Keymod, std::string_view)> editKey;
	void setCaret(std::optional<SDL_FRect> outputRect, float toOutputX, float toOutputY);
	// Direct terminal input, independent of window focus and the OS clipboard.
	bool driveKey(std::string_view description, std::string& error);
	void driveText(std::string_view text);
	// Sends text as a paste (bracketed when the program asked for it).
	void paste(std::string_view text);

private:
	static bool reservedShortcut(SDL_Keycode key, SDL_Keymod mod);
	bool handleShortcut(const SDL_KeyboardEvent& event);
	bool pasteClipboard();
	void textInput(std::string_view text);
	void installPlatform();
	void removePlatform();
#ifdef _WIN32
	bool windowsMessage(MSG& message);
	bool handleWindowsShortcut(const SDL_KeyboardEvent& event);
	bool driveWindowsKey(const SDL_KeyboardEvent& event, std::string& error);
	struct DeferredWindowsKey {
		Uint32 vk = 0, sc = 0, state = 0, repeat = 0;
		std::array<wchar_t, 4> chars{};
		int count = 0;
		bool forward = false;
	};
	// Native Windows scan codes are unprefixed or E0/E1-prefixed, never FFFF.
	static constexpr Uint16 windowsShortcutTag_ = 0xffff;
	std::deque<DeferredWindowsKey> pendingShortcuts_;
	std::array<bool, 256> shortcutKeys_{}, consumedShortcuts_{};
	std::array<bool, 256> consumedKeys_{};
	bool imeComposing_ = false;
	// Characters the last key record already carried; the WM_CHARs TranslateMessage makes from
	// that key repeat them and are dropped (injected keys arrive with scan code 0, so the scan
	// code can't tell them apart from VK_PACKET text).
	int recordedChars_ = 0;
#endif
	SDL_Window& window_;
	std::reference_wrapper<TerminalHost> host_;
	std::reference_wrapper<Pty> pty_;
	bool enabled_ = true;
	bool composing_ = false;
	SDL_Scancode suppressTextKey_ = SDL_SCANCODE_UNKNOWN;
	std::optional<SDL_Rect> inputArea_;
};

} // namespace app
