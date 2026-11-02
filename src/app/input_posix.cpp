#include "input.h"
#include "text_util.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace app {
namespace {

struct ClipboardDeleter {
	void operator()(char* value) const { SDL_free(value); }
};

bool keyNameIs(std::string_view name, std::string_view expected) {
	if (name.size() != expected.size())
		return false;
	for (size_t i = 0; i < name.size(); ++i) {
		const char ch = name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i];
		if (ch != expected[i])
			return false;
	}
	return true;
}

std::string_view trimKeyName(std::string_view name) {
	while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
		name.remove_prefix(1);
	while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
		name.remove_suffix(1);
	return name;
}

bool parseDriveKey(std::string_view description, SDL_KeyboardEvent& event, std::string& error) {
	static constexpr struct {
		std::string_view name;
		SDL_Keycode key;
	} names[] = {{"enter", SDLK_RETURN},    {"return", SDLK_RETURN}, {"esc", SDLK_ESCAPE},
	             {"escape", SDLK_ESCAPE},   {"tab", SDLK_TAB},       {"backspace", SDLK_BACKSPACE},
	             {"space", SDLK_SPACE},     {"plus", SDLK_PLUS},     {"up", SDLK_UP},
	             {"down", SDLK_DOWN},       {"left", SDLK_LEFT},     {"right", SDLK_RIGHT},
	             {"home", SDLK_HOME},       {"end", SDLK_END},       {"insert", SDLK_INSERT},
	             {"ins", SDLK_INSERT},      {"delete", SDLK_DELETE}, {"del", SDLK_DELETE},
	             {"pageup", SDLK_PAGEUP},   {"pgup", SDLK_PAGEUP},   {"pagedown", SDLK_PAGEDOWN},
	             {"pgdown", SDLK_PAGEDOWN}, {"pgdn", SDLK_PAGEDOWN}, {"kp_enter", SDLK_KP_ENTER}};
	description = trimKeyName(description);
	while (!description.empty()) {
		const auto separator = description.find('+');
		std::string_view token;
		if (separator == 0) {
			token = description.substr(0, 1);
			description.remove_prefix(1);
			if (!description.empty()) {
				if (description.front() != '+') {
					error = "Expected '+' between key and modifiers";
					return false;
				}
				description.remove_prefix(1);
				if (description.empty()) {
					error = "Missing key or modifier after '+'";
					return false;
				}
			}
		} else {
			token = trimKeyName(description.substr(0, separator));
			if (separator == std::string_view::npos)
				description = {};
			else {
				description.remove_prefix(separator + 1);
				if (description.empty()) {
					error = "Missing key or modifier after '+'";
					return false;
				}
			}
		}
		SDL_Keymod modifier = SDL_KMOD_NONE;
		if (keyNameIs(token, "ctrl") || keyNameIs(token, "control"))
			modifier = SDL_KMOD_CTRL;
		else if (keyNameIs(token, "alt"))
			modifier = SDL_KMOD_ALT;
		else if (keyNameIs(token, "shift"))
			modifier = SDL_KMOD_SHIFT;
		else if (keyNameIs(token, "cmd") || keyNameIs(token, "command") || keyNameIs(token, "meta"))
			modifier = SDL_KMOD_GUI;
		if (modifier != SDL_KMOD_NONE) {
			event.mod = static_cast<SDL_Keymod>(event.mod | modifier);
			continue;
		}
		SDL_Keycode key = SDLK_UNKNOWN;
		if (token.size() == 1 && token[0] >= ' ' && token[0] <= '~') {
			key = token[0] >= 'A' && token[0] <= 'Z' ? token[0] - 'A' + 'a' : token[0];
		} else {
			for (const auto& named : names) {
				if (keyNameIs(token, named.name)) {
					key = named.key;
					break;
				}
			}
			if (key == SDLK_UNKNOWN && (token.size() == 2 || token.size() == 3) && (token[0] == 'f' || token[0] == 'F')
			    && token[1] >= '1' && token[1] <= '9') {
				const int number =
				    token.size() == 2
				        ? token[1] - '0'
				        : (token[2] >= '0' && token[2] <= '9' ? (token[1] - '0') * 10 + token[2] - '0' : 0);
				if (number >= 1 && number <= 12)
					key = SDLK_F1 + number - 1;
			}
		}
		if (key == SDLK_UNKNOWN) {
			error = "Unknown key: " + std::string(token);
			return false;
		}
		if (event.key != SDLK_UNKNOWN) {
			error = "Key description must contain exactly one key";
			return false;
		}
		event.key = key;
	}
	if (event.key == SDLK_UNKNOWN) {
		error = "Key description must include a key";
		return false;
	}
	SDL_Keymod implied = SDL_KMOD_NONE;
	event.scancode = SDL_GetScancodeFromKey(event.key, &implied);
	event.mod = static_cast<SDL_Keymod>(event.mod | (implied & SDL_KMOD_SHIFT));
	return true;
}

int modifiers(SDL_Keymod mod) {
	return 1 + ((mod & SDL_KMOD_SHIFT) ? 1 : 0) + ((mod & SDL_KMOD_ALT) ? 2 : 0) + ((mod & SDL_KMOD_CTRL) ? 4 : 0)
	     + ((mod & SDL_KMOD_GUI) ? 8 : 0);
}

std::string csiKey(int code, int mod, char final) {
	std::string out = "\x1b[";
	if (code || mod != 1)
		out += std::to_string(code ? code : 1);
	if (mod != 1)
		out += ";" + std::to_string(mod);
	out += final;
	return out;
}

std::string encodeKey(const SDL_KeyboardEvent& event, const TerminalHost& host) {
	const auto key = event.key;
	const int mod = modifiers(event.mod);
	const bool kitty = (host.kittyFlags & 1) != 0;
	const bool alt = (event.mod & SDL_KMOD_ALT) != 0;
	const bool ctrl = (event.mod & SDL_KMOD_CTRL) != 0;
	const bool shift = (event.mod & SDL_KMOD_SHIFT) != 0;
	const bool modified = mod != 1;
	const auto unicode = [&](unsigned code) {
		return "\x1b[" + std::to_string(code) + (modified ? ";" + std::to_string(mod) : "") + "u";
	};
	const auto other = [&](unsigned code) {
		return "\x1b[27;" + std::to_string(mod) + ";" + std::to_string(code) + "~";
	};
	const auto legacy = [&](std::string value) {
		return alt ? "\x1b" + value : value;
	};

	switch (key) {
	case SDLK_UP:
		return csiKey(0, mod, 'A');
	case SDLK_DOWN:
		return csiKey(0, mod, 'B');
	case SDLK_RIGHT:
		return csiKey(0, mod, 'C');
	case SDLK_LEFT:
		return csiKey(0, mod, 'D');
	case SDLK_HOME:
		return csiKey(0, mod, 'H');
	case SDLK_END:
		return csiKey(0, mod, 'F');
	case SDLK_INSERT:
		return csiKey(2, mod, '~');
	case SDLK_DELETE:
		return csiKey(3, mod, '~');
	case SDLK_PAGEUP:
		return csiKey(5, mod, '~');
	case SDLK_PAGEDOWN:
		return csiKey(6, mod, '~');
	case SDLK_F1:
	case SDLK_F2:
	case SDLK_F3:
	case SDLK_F4: {
		const char final = static_cast<char>('P' + key - SDLK_F1);
		if (kitty)
			return key == SDLK_F3 ? csiKey(13, mod, '~') : csiKey(0, mod, final);
		return mod == 1 ? std::string("\x1bO") + final : csiKey(1, mod, final);
	}
	case SDLK_F5:
		return csiKey(15, mod, '~');
	case SDLK_F6:
		return csiKey(17, mod, '~');
	case SDLK_F7:
		return csiKey(18, mod, '~');
	case SDLK_F8:
		return csiKey(19, mod, '~');
	case SDLK_F9:
		return csiKey(20, mod, '~');
	case SDLK_F10:
		return csiKey(21, mod, '~');
	case SDLK_F11:
		return csiKey(23, mod, '~');
	case SDLK_F12:
		return csiKey(24, mod, '~');
	case SDLK_ESCAPE:
		if (kitty)
			return unicode(27);
		if (host.modifyOtherKeys && modified)
			return other(27);
		return legacy("\x1b");
	case SDLK_KP_ENTER:
		if (kitty)
			return unicode(57414);
		[[fallthrough]];
	case SDLK_RETURN:
		if (kitty && modified)
			return unicode(13);
		if (host.modifyOtherKeys && modified)
			return other(13);
		return legacy(ctrl ? "\n" : "\r");
	case SDLK_TAB:
		if (kitty && modified)
			return unicode(9);
		if (host.modifyOtherKeys && modified)
			return other(9);
		return shift ? "\x1b[Z" : legacy("\t");
	case SDLK_BACKSPACE:
		if (kitty && modified)
			return unicode(127);
		if (host.modifyOtherKeys && modified)
			return other(127);
		return legacy(ctrl ? "\b" : "\x7f");
	default:
		break;
	}

	if (event.mod & SDL_KMOD_MODE)
		return {}; // AltGr text belongs to the input method.
	// Ordinary printable input (including dead keys and composed text) arrives
	// through SDL_TEXT_INPUT. Only encode the combinations SDL cannot preserve.
	if (!ctrl && !alt && !(event.mod & SDL_KMOD_GUI))
		return {};
	if (key & SDLK_SCANCODE_MASK)
		return {};
	if (key < 0x20 || key > 0x10ffff)
		return {};
	if (kitty)
		return unicode(key);
	const auto translated = SDL_GetKeyFromScancode(
	    event.scancode, static_cast<SDL_Keymod>(event.mod & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS)), false);
	const auto code = translated && !(translated & SDLK_SCANCODE_MASK) ? translated : key;
	if (host.modifyOtherKeys)
		return other(code);
	if (ctrl) {
		if (key >= SDLK_A && key <= SDLK_Z)
			return legacy(std::string(1, static_cast<char>(key - SDLK_A + 1)));
		if (key == SDLK_SPACE || key == SDLK_2 || key == SDLK_AT)
			return legacy(std::string(1, '\0'));
		if (key >= SDLK_LEFTBRACKET && key <= SDLK_UNDERSCORE)
			return legacy(std::string(1, static_cast<char>(key - 64)));
		if (key >= SDLK_3 && key <= SDLK_7)
			return legacy(std::string(1, static_cast<char>(key - SDLK_3 + 27)));
		if (key == SDLK_8 || key == SDLK_QUESTION)
			return legacy("\x7f");
	}
	if (alt) {
		std::string out = "\x1b";
		text::append(out, code);
		return out;
	}
	return {};
}

} // namespace

Input::Input(SDL_Window& window, TerminalHost& host, Pty& pty) : window_(window), host_(host), pty_(pty) {
	installPlatform();
	SDL_StartTextInput(&window_);
}

Input::~Input() {
	SDL_StopTextInput(&window_);
	removePlatform();
}

#ifndef _WIN32
void Input::installPlatform() {}
void Input::removePlatform() {}
void Input::textInput(std::string_view value) {
	pty_.get().write(value);
}
#endif

bool Input::reservedShortcut(SDL_Keycode key, SDL_Keymod mod) {
	if (mod & SDL_KMOD_MODE)
		return false;
#ifdef __APPLE__
	if (!(mod & SDL_KMOD_GUI) || (mod & SDL_KMOD_CTRL))
		return false;
	if (mod & SDL_KMOD_ALT)
		return (mod & SDL_KMOD_SHIFT) && (key == SDLK_T || key == SDLK_M);
	if (mod & SDL_KMOD_SHIFT)
		return key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET;
	return key == SDLK_F || key == SDLK_T || key == SDLK_W || key == SDLK_B || (key >= SDLK_1 && key <= SDLK_9);
#else
	if (!(mod & SDL_KMOD_CTRL) || (mod & SDL_KMOD_GUI))
		return false;
	if (mod & SDL_KMOD_ALT)
		return (mod & SDL_KMOD_SHIFT) && (key == SDLK_T || key == SDLK_M);
	if (key == SDLK_TAB)
		return true;
	if (!(mod & SDL_KMOD_SHIFT))
		return key == SDLK_PAGEUP || key == SDLK_PAGEDOWN;
	return key == SDLK_F || key == SDLK_T || key == SDLK_W || key == SDLK_B || (key >= SDLK_1 && key <= SDLK_9);
#endif
}

bool Input::handleShortcut(const SDL_KeyboardEvent& event) {
	if (!onShortcut
	    || !(reservedShortcut(event.key, event.mod) || claimed(event.key, event.mod) || event.key == SDLK_F12
	         || event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER))
		return false;
	// Closing/merging a window can destroy this Input during the callback.
	const auto shortcut = onShortcut;
	return shortcut(event);
}

bool Input::driveKey(std::string_view description, std::string& error) {
	error.clear();
	SDL_KeyboardEvent event{};
	event.type = SDL_EVENT_KEY_DOWN;
	event.windowID = SDL_GetWindowID(&window_);
	event.down = true;
	if (!parseDriveKey(description, event, error))
		return false;
	if (searchKey && claimSearchKey && claimSearchKey(event.key, event.mod)) {
		std::string typed;
		if (event.key >= SDLK_SPACE && event.key <= SDLK_TILDE
		    && !(event.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT))) {
			const auto translated =
			    SDL_GetKeyFromScancode(event.scancode, static_cast<SDL_Keymod>(event.mod & SDL_KMOD_SHIFT), false);
			text::append(typed, translated && !(translated & SDLK_SCANCODE_MASK) ? translated : event.key);
		}
		if (searchKey(event.key, event.mod, typed))
			return true;
	}
	if (handleShortcut(event))
		return true;
	if (editKey && editKey(event.key, event.mod, {}))
		return true;
#ifdef _WIN32
	if (host_.get().win32InputMode)
		return driveWindowsKey(event, error);
#endif
	const auto bytes = encodeKey(event, host_.get());
	if (!bytes.empty())
		pty_.get().write(bytes);
	else if (event.key >= SDLK_SPACE && event.key <= SDLK_TILDE) {
		const auto translated =
		    SDL_GetKeyFromScancode(event.scancode, static_cast<SDL_Keymod>(event.mod & SDL_KMOD_SHIFT), false);
		const auto code = translated && !(translated & SDLK_SCANCODE_MASK) ? translated : event.key;
		std::string value;
		text::append(value, code);
		textInput(value);
	}
	return true;
}

void Input::driveText(std::string_view value) {
	if (searchText && searchText(value))
		return;
	if (editKey && editKey(SDLK_UNKNOWN, SDL_KMOD_NONE, value))
		return;
	textInput(value);
}

void Input::paste(std::string_view source) {
	if (searchText && searchText(source))
		return;
	if (source.empty())
		return;
	std::string text;
	text.reserve(source.size() + (host_.get().bracketedPaste ? 12 : 0));
	if (host_.get().bracketedPaste)
		text += "\x1b[200~";
	for (size_t i = 0; i < source.size(); ++i) {
		if (source[i] == '\r' && i + 1 < source.size() && source[i + 1] == '\n')
			continue;
		text += source[i] == '\n' ? '\r' : source[i];
	}
	if (host_.get().bracketedPaste)
		text += "\x1b[201~";
	pty_.get().write(text);
}

bool Input::pasteClipboard() {
	std::unique_ptr<char, ClipboardDeleter> clipboard(SDL_GetClipboardText());
	if (!clipboard || !*clipboard)
		return false;
	paste(clipboard.get());
	return true;
}

bool Input::handle(const SDL_Event& event) {
	if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
		if (event.window.windowID != SDL_GetWindowID(&window_))
			return false;
		suppressTextKey_ = SDL_SCANCODE_UNKNOWN;
		composing_ = false;
		return false;
	}
	if (event.type == SDL_EVENT_TEXT_EDITING) {
		if (event.edit.windowID != SDL_GetWindowID(&window_))
			return false;
		composing_ = event.edit.text && *event.edit.text;
		return true;
	}
	if (event.type == SDL_EVENT_TEXT_INPUT) {
		if (event.text.windowID != SDL_GetWindowID(&window_))
			return false;
		if (suppressTextKey_ == SDL_SCANCODE_UNKNOWN || composing_) {
			if (searchText && searchText(event.text.text)) {
				suppressTextKey_ = SDL_SCANCODE_UNKNOWN;
				composing_ = false;
				return true;
			}
			if (enabled_) {
				if (!host_.get().win32InputMode && !composing_ && (SDL_GetModState() & SDL_KMOD_ALT)
				    && !(SDL_GetModState() & SDL_KMOD_MODE))
					pty_.get().write("\x1b");
				if (!editKey || !editKey(SDLK_UNKNOWN, SDL_KMOD_NONE, event.text.text))
					textInput(event.text.text);
			}
		}
		suppressTextKey_ = SDL_SCANCODE_UNKNOWN;
		composing_ = false;
		return true;
	}
	if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
		return false;
	if (event.key.windowID != SDL_GetWindowID(&window_))
		return false;
#ifdef _WIN32
	if (event.key.raw == windowsShortcutTag_)
		return handleWindowsShortcut(event.key);
	// Windows key records already left through the raw message hook. SDL text
	// commits remain enabled; duplicate WM_CHAR messages are dropped in the hook.
	if (host_.get().win32InputMode)
		return true;
#endif
	if (event.type == SDL_EVENT_KEY_UP) {
		if (event.key.scancode == suppressTextKey_)
			suppressTextKey_ = SDL_SCANCODE_UNKNOWN;
		return true;
	}
	if (composing_ && searchFocused && searchFocused())
		return true;
	if (searchKey && searchKey(event.key.key, event.key.mod, {})) {
		// Local command chords must not also insert the text SDL emits for their letter.
		const auto mod = event.key.mod;
		const bool chord = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))
		                || ((mod & SDL_KMOD_ALT) && (event.key.key == SDLK_C || event.key.key == SDLK_R));
		suppressTextKey_ = chord && !(mod & SDL_KMOD_MODE) ? event.key.scancode : SDL_SCANCODE_UNKNOWN;
		return true;
	}
	suppressTextKey_ = event.key.scancode;
	if (handleShortcut(event.key))
		return true;
	suppressTextKey_ = SDL_SCANCODE_UNKNOWN;
	if (!enabled_ || composing_)
		return true;
	if (editKey && editKey(event.key.key, event.key.mod, {}))
		return true;
	const auto mod = event.key.mod;
	const bool ctrlV = (mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_ALT) && event.key.key == SDLK_V;
	const bool shiftInsert = (mod & SDL_KMOD_SHIFT) && event.key.key == SDLK_INSERT;
#ifdef __APPLE__
	const bool cmdV = (mod & SDL_KMOD_GUI) && event.key.key == SDLK_V;
#else
	const bool cmdV = false;
#endif
	if ((ctrlV || shiftInsert || cmdV) && pasteClipboard()) {
		suppressTextKey_ = event.key.scancode;
		return true;
	}
	const auto bytes = encodeKey(event.key, host_.get());
	if (!bytes.empty()) {
		pty_.get().write(bytes);
		suppressTextKey_ = event.key.scancode;
	}
	return true;
}

void Input::setCaret(std::optional<SDL_FRect> rect, float toOutputX, float toOutputY) {
	std::optional<SDL_Rect> area;
	if (rect) {
		const int x = static_cast<int>(std::floor(rect->x / toOutputX));
		const int y = static_cast<int>(std::floor(rect->y / toOutputY));
		area = SDL_Rect{x, y, std::max(1, static_cast<int>(std::ceil((rect->x + rect->w) / toOutputX)) - x),
		                std::max(1, static_cast<int>(std::ceil((rect->y + rect->h) / toOutputY)) - y)};
	}
	if (area.has_value() == inputArea_.has_value() && (!area || SDL_RectsEqual(&*area, &*inputArea_)))
		return;
	inputArea_ = area;
	SDL_SetTextInputArea(&window_, inputArea_ ? &*inputArea_ : nullptr, 0);
}

} // namespace app
