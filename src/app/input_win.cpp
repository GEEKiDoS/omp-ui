#include "input.h"

#include <SDL3/SDL_system.h>
#include <windows.h>
#include <imm.h>

#include "text_util.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace app {
namespace {
std::vector<std::reference_wrapper<Input>> windowsInputs;

void appendWin32Record(std::string& out, UINT vk, UINT sc, unsigned uc, bool down, DWORD cs, UINT repeat) {
	std::array<char, 96> buffer{};
	const int size =
	    std::snprintf(buffer.data(), buffer.size(), "\x1b[%u;%u;%u;%d;%lu;%u_", vk, sc, uc, down ? 1 : 0, cs, repeat);
	out.append(buffer.data(), size);
}

void appendKeyRecords(std::string& out, UINT vk, UINT sc, bool down, DWORD state, UINT repeat,
                      const std::array<BYTE, 256>& keys) {
	std::array<wchar_t, 4> chars{};
	// Don't disturb dead-key state: SDL's TranslateMessage still owns it.
	const int count =
	    ToUnicode(vk, sc | (down ? 0 : 0x8000), keys.data(), chars.data(), static_cast<int>(chars.size()), 4);
	if (count > 0) {
		for (int i = 0; i < count; ++i)
			appendWin32Record(out, vk, sc, chars[i], down, state, repeat);
	} else
		appendWin32Record(out, vk, sc, 0, down, state, repeat);
}

DWORD controlKeyState(bool extended) {
	DWORD state = 0;
	if (GetKeyState(VK_RMENU) & 0x8000)
		state |= RIGHT_ALT_PRESSED;
	if (GetKeyState(VK_LMENU) & 0x8000)
		state |= LEFT_ALT_PRESSED;
	if (GetKeyState(VK_RCONTROL) & 0x8000)
		state |= RIGHT_CTRL_PRESSED;
	if (GetKeyState(VK_LCONTROL) & 0x8000)
		state |= LEFT_CTRL_PRESSED;
	if (GetKeyState(VK_SHIFT) & 0x8000)
		state |= SHIFT_PRESSED;
	if (GetKeyState(VK_NUMLOCK) & 1)
		state |= NUMLOCK_ON;
	if (GetKeyState(VK_SCROLL) & 1)
		state |= SCROLLLOCK_ON;
	if (GetKeyState(VK_CAPITAL) & 1)
		state |= CAPSLOCK_ON;
	if (extended)
		state |= ENHANCED_KEY;
	return state;
}

// An uncommitted IME composition exists (TSF IMEs answer through IMM as well).
bool imeComposition(HWND hwnd) {
	const HIMC context = ImmGetContext(hwnd);
	if (!context)
		return false;
	const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
	ImmReleaseContext(hwnd, context);
	return bytes > 0;
}

} // namespace

void Input::installPlatform() {
	windowsInputs.emplace_back(*this);
	if (windowsInputs.size() != 1)
		return;
	SDL_SetWindowsMessageHook(
	    [](void*, MSG* message) -> bool {
		    for (auto& input : windowsInputs)
			    if (!input.get().windowsMessage(*message))
				    return false;
		    return true;
	    },
	    nullptr);
}

void Input::removePlatform() {
	std::erase_if(windowsInputs, [this](const auto& input) { return &input.get() == this; });
	if (windowsInputs.empty())
		SDL_SetWindowsMessageHook(nullptr, nullptr);
}

bool Input::windowsMessage(MSG& message) {
	const auto hwnd = static_cast<HWND>(
	    SDL_GetPointerProperty(SDL_GetWindowProperties(&window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	if (message.hwnd != hwnd)
		return true;
	const bool down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
	const bool up = message.message == WM_KEYUP || message.message == WM_SYSKEYUP;
	const UINT vk = static_cast<UINT>(message.wParam);
	const bool consumed = (down || up) && vk < consumedKeys_.size() && consumedKeys_[vk];
	const bool deferred = (down || up) && vk < shortcutKeys_.size() && shortcutKeys_[vk];
	if (up && consumed) {
		consumedKeys_[vk] = false;
		return false;
	}
	if (up && vk < searchKeys_.size() && searchKeys_[vk]) {
		searchKeys_[vk] = false;
		return true; // SDL still needs modifier releases, but omp never owned this key.
	}
	if (!host_.get().win32InputMode && !deferred && !consumed)
		return true;
	// SDL's IME backend emits TEXT_INPUT directly from WM_IME_COMPOSITION. WM_CHARs made from a
	// key we sent as a record repeat it; others (VK_PACKET/Unicode injection, non-key commits)
	// still go through SDL text.
	if (message.message == WM_CHAR || message.message == WM_SYSCHAR) {
		if (recordedChars_ <= 0)
			return true;
		--recordedChars_;
		return false;
	}
	if (!down && !up)
		return true;
	if (vk == VK_PROCESSKEY || vk == VK_PACKET || vk >= consumedKeys_.size())
		return true;
	// Asked of the IME itself: WM_IME_STARTCOMPOSITION/ENDCOMPOSITION may be sent straight to the
	// window procedure rather than posted, so this hook can see one without the other.
	const bool imeComposing = imeComposition(hwnd);
	const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
	const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	const bool gui = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
	const UINT sc = (message.lParam >> 16) & 0xff;
	const bool extended = ((message.lParam >> 24) & 1) != 0;
	SDL_Keycode key = vk >= 'A' && vk <= 'Z' ? SDLK_A + vk - 'A' : vk >= '0' && vk <= '9' ? vk : SDLK_UNKNOWN;
	if (vk == VK_TAB)
		key = SDLK_TAB;
	if (vk == VK_PRIOR)
		key = SDLK_PAGEUP;
	if (vk == VK_NEXT)
		key = SDLK_PAGEDOWN;
	if (vk == VK_RETURN)
		key = extended ? SDLK_KP_ENTER : SDLK_RETURN;
	if (vk >= VK_F1 && vk <= VK_F12)
		key = SDLK_F1 + vk - VK_F1;
	if (vk == VK_BACK)
		key = SDLK_BACKSPACE;
	if (vk == VK_DELETE)
		key = SDLK_DELETE;
	if (vk == VK_ESCAPE)
		key = SDLK_ESCAPE;
	if (vk == VK_SPACE)
		key = SDLK_SPACE;
	if (vk == VK_LEFT)
		key = SDLK_LEFT;
	if (vk == VK_RIGHT)
		key = SDLK_RIGHT;
	if (vk == VK_UP)
		key = SDLK_UP;
	if (vk == VK_DOWN)
		key = SDLK_DOWN;
	if (vk == VK_HOME)
		key = SDLK_HOME;
	if (vk == VK_END)
		key = SDLK_END;
	if (vk == VK_INSERT)
		key = SDLK_INSERT;
	const auto mod = static_cast<SDL_Keymod>((ctrl ? SDL_KMOD_CTRL : 0) | (alt ? SDL_KMOD_ALT : 0)
	                                         | (shift ? SDL_KMOD_SHIFT : 0) | (gui ? SDL_KMOD_GUI : 0));
	const bool local =
	    searchPending_ || (searchFocused && searchFocused()) || (claimSearchKey && claimSearchKey(key, mod));
	const bool modifierKey =
	    vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL;
	if (local && !deferred && (modifierKey || imeComposing)) {
		searchKeys_[vk] = down;
		recordedChars_ = 0;
		return true; // Keep SDL's modifier/IME state without sending terminal records.
	}
	if (deferred || local
	    || (down && onShortcut
	        && (reservedShortcut(key, mod) || claimed(key, mod) || key == SDLK_F12 || key == SDLK_RETURN
	            || key == SDLK_KP_ENTER))) {
		SDL_Event event{};
		event.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.timestamp = SDL_GetTicksNS();
		event.key.windowID = SDL_GetWindowID(&window_);
		event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
		event.key.key = key;
		event.key.mod = mod;
		event.key.raw = windowsShortcutTag_;
		event.key.down = down;
		event.key.repeat = down && ((message.lParam >> 30) & 1) != 0;
		pendingShortcuts_.emplace_back();
		auto& pending = pendingShortcuts_.back();
		pending.vk = vk;
		pending.sc = sc;
		pending.state = controlKeyState(extended);
		pending.repeat = down ? std::max<UINT>(1, message.lParam & 0xffff) : 1;
		pending.search = local;
		pending.forward = host_.get().win32InputMode && enabled_ && !imeComposing;
		if (pending.forward || (pending.search && down)) {
			std::array<BYTE, 256> keys{};
			GetKeyboardState(keys.data());
			// Local keys never reach TranslateMessage, so own their dead-key state here.
			pending.count = ToUnicode(vk, sc | (down ? 0 : 0x8000), keys.data(), pending.chars.data(),
			                          static_cast<int>(pending.chars.size()), pending.search && down ? 0 : 4);
		}
		shortcutKeys_[vk] = down;
		// Wake the normal SDL dispatcher without translating a duplicate key or
		// text event. App callbacks can destroy the active tab, so cannot run here.
		if (!SDL_PushEvent(&event))
			pendingShortcuts_.pop_back();
		else if (local && down && key == SDLK_F && ctrl && shift && !alt && !gui)
			searchPending_ = true; // Capture keys queued before the open shortcut is dispatched.
		if (local)
			recordedChars_ = 0;
		return false;
	}
	if (consumed)
		return false;
	if (imeComposing || !enabled_)
		return true;
	if (down && editKey && !modifierKey) {
		// The typed text (without disturbing dead-key state) lets a selected editor range be replaced.
		std::array<BYTE, 256> keys{};
		GetKeyboardState(keys.data());
		std::array<wchar_t, 4> chars{};
		const int count = ToUnicode(vk, sc, keys.data(), chars.data(), static_cast<int>(chars.size()), 4);
		std::string typed;
		for (int i = 0; i < count; ++i) {
			uint32_t unit = chars[i];
			if (unit >= 0xd800 && unit < 0xdc00 && i + 1 < count)
				unit = 0x10000 + ((unit - 0xd800) << 10) + (chars[++i] - 0xdc00);
			if (unit >= 0x20)
				text::append(typed, unit);
		}
		if (editKey(key, mod, typed)) {
			consumedKeys_[vk] = true;
			return false;
		}
	}
	if (down && ((ctrl && !alt && vk == 'V') || (shift && vk == VK_INSERT)) && pasteClipboard()) {
		consumedKeys_[vk] = true;
		return false;
	}
	std::array<BYTE, 256> keys{};
	GetKeyboardState(keys.data());
	const DWORD state = controlKeyState(extended);
	const UINT repeat = down ? std::max<UINT>(1, message.lParam & 0xffff) : 1;
	std::string out;
	appendKeyRecords(out, vk, sc, down, state, repeat, keys);
	pty_.get().write(out);
	if (down) {
		// Flag 4 leaves the dead-key state alone; a dead key (-1) produces no WM_CHAR.
		std::array<wchar_t, 4> chars{};
		recordedChars_ = std::max(0, ToUnicode(vk, sc, keys.data(), chars.data(), static_cast<int>(chars.size()), 4));
	}
	// Continue so SDL maintains modifier state and lets the IME process keys.
	return true;
}

bool Input::handleWindowsShortcut(const SDL_KeyboardEvent& event) {
	const auto pending = pendingShortcuts_.front();
	pendingShortcuts_.pop_front();
	auto key = event;
	key.raw = static_cast<Uint16>(pending.sc | ((pending.state & ENHANCED_KEY) ? 0xe000 : 0));
	if (pending.search && event.type == SDL_EVENT_KEY_DOWN && key.key == SDLK_F && (key.mod & SDL_KMOD_CTRL)
	    && (key.mod & SDL_KMOD_SHIFT) && !(key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI)))
		searchPending_ = false;
	if (event.type == SDL_EVENT_KEY_DOWN) {
		const bool consumed = event.repeat && consumedShortcuts_[pending.vk];
		consumedShortcuts_[pending.vk] = true;
		if (composing_ && searchFocused && searchFocused())
			return true;
		std::string typed;
		if (pending.search && !composing_) {
			for (int i = 0; i < pending.count; ++i) {
				uint32_t unit = pending.chars[i];
				if (unit >= 0xd800 && unit < 0xdc00 && i + 1 < pending.count)
					unit = 0x10000 + ((unit - 0xd800) << 10) + (pending.chars[++i] - 0xdc00);
				if (unit >= 0x20 && unit != 0x7f)
					text::append(typed, unit);
			}
		}
		if (searchKey && searchKey(key.key, key.mod, typed)) {
			suppressTextKey_ = SDL_SCANCODE_UNKNOWN;
			return true;
		}
		if (handleShortcut(key))
			return true;
		consumedShortcuts_[pending.vk] = consumed;
		if (consumed)
			return true;
		if (enabled_ && !composing_ && editKey && editKey(key.key, key.mod, typed)) {
			consumedShortcuts_[pending.vk] = true;
			return true;
		}
		const bool ctrlV = (key.mod & SDL_KMOD_CTRL) && !(key.mod & SDL_KMOD_ALT) && key.key == SDLK_V;
		const bool shiftInsert = (key.mod & SDL_KMOD_SHIFT) && key.key == SDLK_INSERT;
		if (enabled_ && (ctrlV || shiftInsert) && pasteClipboard()) {
			consumedShortcuts_[pending.vk] = true;
			return true;
		}
	} else {
		const bool consumed = consumedShortcuts_[pending.vk];
		consumedShortcuts_[pending.vk] = false;
		if (consumed)
			return true;
	}
	if (searchFocused && searchFocused())
		return true;
	if (!pending.forward || !enabled_)
		return true;
	std::string out;
	if (pending.count > 0) {
		for (int i = 0; i < pending.count; ++i)
			appendWin32Record(out, pending.vk, pending.sc, pending.chars[i], event.down, pending.state, pending.repeat);
	} else
		appendWin32Record(out, pending.vk, pending.sc, 0, event.down, pending.state, pending.repeat);
	pty_.get().write(out);
	return true;
}

bool Input::driveWindowsKey(const SDL_KeyboardEvent& event, std::string& error) {
	UINT vk = 0;
	DWORD state = 0;
	if (event.mod & SDL_KMOD_CTRL)
		state |= LEFT_CTRL_PRESSED;
	if (event.mod & SDL_KMOD_ALT)
		state |= LEFT_ALT_PRESSED;
	if (event.mod & SDL_KMOD_SHIFT)
		state |= SHIFT_PRESSED;
	if (event.key >= SDLK_SPACE && event.key <= SDLK_TILDE) {
		const SHORT mapped = VkKeyScanW(static_cast<wchar_t>(event.key));
		if (mapped == -1) {
			error = "Key is unavailable in the current keyboard layout";
			return false;
		}
		vk = mapped & 0xff;
		if (mapped & 0x0100)
			state |= SHIFT_PRESSED;
		if (mapped & 0x0200)
			state |= LEFT_CTRL_PRESSED;
		if (mapped & 0x0400)
			state |= LEFT_ALT_PRESSED;
	} else if (event.key >= SDLK_F1 && event.key <= SDLK_F12) {
		vk = VK_F1 + event.key - SDLK_F1;
	} else {
		switch (event.key) {
		case SDLK_RETURN:
			vk = VK_RETURN;
			break;
		case SDLK_KP_ENTER:
			vk = VK_RETURN;
			state |= ENHANCED_KEY;
			break;
		case SDLK_ESCAPE:
			vk = VK_ESCAPE;
			break;
		case SDLK_TAB:
			vk = VK_TAB;
			break;
		case SDLK_BACKSPACE:
			vk = VK_BACK;
			break;
		case SDLK_UP:
			vk = VK_UP;
			break;
		case SDLK_DOWN:
			vk = VK_DOWN;
			break;
		case SDLK_LEFT:
			vk = VK_LEFT;
			break;
		case SDLK_RIGHT:
			vk = VK_RIGHT;
			break;
		case SDLK_HOME:
			vk = VK_HOME;
			break;
		case SDLK_END:
			vk = VK_END;
			break;
		case SDLK_INSERT:
			vk = VK_INSERT;
			break;
		case SDLK_DELETE:
			vk = VK_DELETE;
			break;
		case SDLK_PAGEUP:
			vk = VK_PRIOR;
			break;
		case SDLK_PAGEDOWN:
			vk = VK_NEXT;
			break;
		default:
			error = "Unknown Windows key";
			return false;
		}
	}
	const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
	if (scan & 0xff00)
		state |= ENHANCED_KEY;
	const UINT sc = scan & 0xff;
	// Synthetic input never reads or changes the OS keyboard's modifier state.
	std::array<BYTE, 256> keys{};
	if (state & LEFT_CTRL_PRESSED)
		keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
	if (state & LEFT_ALT_PRESSED)
		keys[VK_MENU] = keys[VK_LMENU] = 0x80;
	if (state & SHIFT_PRESSED)
		keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
	keys[vk] = 0x80;
	std::string out;
	appendKeyRecords(out, vk, sc, true, state, 1, keys);
	keys[vk] = 0;
	appendKeyRecords(out, vk, sc, false, state, 1, keys);
	pty_.get().write(out);
	return true;
}

void Input::textInput(std::string_view value) {
	if (!host_.get().win32InputMode) {
		pty_.get().write(value);
		return;
	}
	std::string out;
	const auto unit = [&](unsigned ch) {
		appendWin32Record(out, 0, 0, ch, true, 0, 1);
		appendWin32Record(out, 0, 0, ch, false, 0, 1);
	};
	for (size_t i = 0; i < value.size();) {
		const auto cp = text::decode(value, i);
		if (cp < 0x10000)
			unit(cp);
		else {
			unit(0xd800 + ((cp - 0x10000) >> 10));
			unit(0xdc00 + ((cp - 0x10000) & 0x3ff));
		}
	}
	pty_.get().write(out);
}

} // namespace app
