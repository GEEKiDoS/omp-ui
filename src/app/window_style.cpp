#include "window_style.h"

#include "text/spans.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <span>
#include <string>
#endif

namespace app {

#ifdef _WIN32
namespace {

constexpr wchar_t previousProc[] = L"omp-ui.previous-wndproc";
// The notification tray icon's callback message; a window's icon exists once it has notified.
constexpr UINT trayMessage = WM_APP + 77;
constexpr wchar_t trayProp[] = L"omp-ui.tray";

NOTIFYICONDATAW trayIcon(HWND hwnd) {
	return {.cbSize = sizeof(NOTIFYICONDATAW), .hWnd = hwnd, .uID = 1};
}

// Windows 11 is build 22000. GetVersionEx reports what the manifest declares, RtlGetVersion
// the real build.
DWORD windowsBuild() {
	using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
	static const DWORD build = [] {
		OSVERSIONINFOW info{};
		info.dwOSVersionInfoSize = sizeof(info);
		const auto ntdll = GetModuleHandleW(L"ntdll.dll");
		const auto get = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
		return get && get(&info) == 0 ? info.dwBuildNumber : DWORD{0};
	}();
	return build;
}
bool windows10() {
	const auto build = windowsBuild();
	return build != 0 && build < 22000;
}

// The client area covers the whole window, so omp-ui draws its own caption, while the window
// keeps its captioned style: DWM still gives it a shadow, rounded corners, snapping and the
// minimise/maximise animations, which a WS_POPUP window loses.
LRESULT CALLBACK frameProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	const auto previous = reinterpret_cast<WNDPROC>(GetPropW(hwnd, previousProc));
	if (message == WM_NCCALCSIZE && wParam == TRUE) {
		// Maximised, Windows oversizes the window by its frame; keep the client on the work area.
		if (IsZoomed(hwnd)) {
			auto& params = *reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
			MONITORINFO info{};
			info.cbSize = sizeof(info);
			if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
				params.rgrc[0] = info.rcWork;
		}
		return 0;
	}
	// Clicking the notification (or the tray icon) brings the window to the front.
	if (message == trayMessage && (LOWORD(lParam) == NIN_BALLOONUSERCLICK || LOWORD(lParam) == WM_LBUTTONUP)) {
		if (IsIconic(hwnd))
			ShowWindow(hwnd, SW_RESTORE);
		SetForegroundWindow(hwnd);
		return 0;
	}
	if (message == WM_NCDESTROY) {
		RemovePropW(hwnd, previousProc);
		if (RemovePropW(hwnd, trayProp)) {
			auto data = trayIcon(hwnd);
			Shell_NotifyIconW(NIM_DELETE, &data);
		}
	}
	return CallWindowProcW(previous, hwnd, message, wParam, lParam);
}

void removeFrame(HWND hwnd, bool translucent) {
	if (!GetPropW(hwnd, previousProc)) {
		SetPropW(hwnd, previousProc, reinterpret_cast<HANDLE>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)));
		SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(frameProc));
	}
	// A one-pixel frame inside the client keeps DWM drawing the shadow. On Windows 10 that frame
	// strip shows through a translucent (blurred) window as a white bar, so those go without frame
	// rendering, and so without the shadow; Windows 11 draws neither the bar nor its border there.
	const bool frame = !(translucent && windows10());
	const DWMNCRENDERINGPOLICY policy = frame ? DWMNCRP_ENABLED : DWMNCRP_DISABLED;
	DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
	const MARGINS margins{0, 0, 0, frame ? 1 : 0};
	DwmExtendFrameIntoClientArea(hwnd, &margins);
	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
	             SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
}

// user32's window composition accent (undocumented, stable since Windows 10 1803): blurs what
// lies behind the window's translucent pixels over the whole window. The documented system
// backdrops (DWMWA_SYSTEMBACKDROP_TYPE) need the frame extended over the client, which makes DWM
// draw its caption buttons and leaves the caption area unblurred. Windows draws neither while
// power saving or transparency effects are off.
struct AccentPolicy {
	int state;
	DWORD flags;
	DWORD tint; // 0xAABBGGRR
	DWORD animation;
};
struct CompositionData {
	int attribute;
	void* data;
	SIZE_T size;
};
using SetCompositionFn = BOOL(WINAPI*)(HWND, CompositionData*);

void setAccent(HWND hwnd, Theme::Backdrop backdrop) {
	const auto user32 = GetModuleHandleW(L"user32.dll");
	const auto set =
	    user32 ? reinterpret_cast<SetCompositionFn>(GetProcAddress(user32, "SetWindowCompositionAttribute")) : nullptr;
	if (!set)
		return;
	constexpr int disabled = 0, blur = 3, acrylic = 4, accentPolicy = 19;
	// Acrylic needs a tint with a little alpha; the canvas hue keeps the theme's tone.
	const auto canvas = text::palette::canvas;
	const DWORD tint = 0x01000000u | (DWORD{canvas.b} << 16) | (DWORD{canvas.g} << 8) | DWORD{canvas.r};
	const int state = backdrop == Theme::Backdrop::Acrylic ? acrylic
	                : backdrop == Theme::Backdrop::Blur    ? blur
	                                                       : disabled;
	AccentPolicy policy{state, 0, tint, 0};
	CompositionData data{accentPolicy, &policy, sizeof(policy)};
	set(hwnd, &data);
}

} // namespace

SDL_WindowFlags frameFlags() {
	return 0;
}

void styleWindow(SDL_Window& window, const Theme& theme, bool translucent) {
	const auto hwnd = static_cast<HWND>(
	    SDL_GetPointerProperty(SDL_GetWindowProperties(&window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	if (!hwnd)
		return;
	// DWMWA_USE_IMMERSIVE_DARK_MODE, by number so older SDK headers still build: 20 since
	// Windows 10 20H1, 19 before it. It also darkens Windows 10's window border.
	const BOOL dark = text::darkPalette;
	if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
		DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
	removeFrame(hwnd, translucent && theme.transparent());
	// Disabled again (Backdrop::None) when a reloaded theme drops it.
	setAccent(hwnd, translucent && theme.transparent() ? theme.backdrop : Theme::Backdrop::None);
}

void notify(SDL_Window& window, bool toast, std::string_view title, std::string_view message) {
	SDL_FlashWindow(&window, SDL_FLASH_UNTIL_FOCUSED);
	const auto hwnd = static_cast<HWND>(
	    SDL_GetPointerProperty(SDL_GetWindowProperties(&window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	if (!toast) {
		MessageBeep(MB_ICONINFORMATION);
		return;
	}
	// A tray icon's balloon is a regular Windows 10/11 notification (sound included) without the
	// app registration a toast needs. The icon stays (hidden in the overflow) while the window lives.
	auto data = trayIcon(hwnd);
	data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
	data.uCallbackMessage = trayMessage;
	data.hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
	if (!data.hIcon)
		data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
	// Fixed-size fields: convert, drop private-use characters (omp titles start with Nerd Font
	// icons, which no system font has, so Windows shows a box) and the spaces they leave at either
	// end, then cut to fit without splitting a surrogate pair.
	const auto copy = [](std::string_view from, std::span<wchar_t> to) {
		std::wstring wide(static_cast<size_t>(
		                      MultiByteToWideChar(CP_UTF8, 0, from.data(), static_cast<int>(from.size()), nullptr, 0)),
		                  L'\0');
		MultiByteToWideChar(CP_UTF8, 0, from.data(), static_cast<int>(from.size()), wide.data(),
		                    static_cast<int>(wide.size()));
		size_t kept = 0;
		for (size_t i = 0; i < wide.size(); ++i) {
			const wchar_t unit = wide[i];
			if (unit >= 0xE000 && unit <= 0xF8FF)
				continue;
			if (unit >= 0xDB80 && unit <= 0xDBFF) { // U+F0000 and up: the supplementary private-use planes.
				++i;
				continue;
			}
			wide[kept++] = unit;
		}
		wide.resize(kept);
		const auto first = wide.find_first_not_of(L' ');
		const auto text = first == std::wstring::npos
		                    ? std::wstring_view()
		                    : std::wstring_view(wide).substr(first, wide.find_last_not_of(L' ') - first + 1);
		size_t length = std::min(text.size(), to.size() - 1);
		if (length < text.size() && length > 0 && IS_HIGH_SURROGATE(text[length - 1]))
			--length;
		std::copy_n(text.begin(), length, to.begin());
		to[length] = L'\0';
	};
	copy(title, data.szInfoTitle);
	copy(message, data.szInfo);
	copy("omp-ui", data.szTip);
	// The first notification adds the icon, later ones update it.
	if (!GetPropW(hwnd, trayProp) && Shell_NotifyIconW(NIM_ADD, &data))
		SetPropW(hwnd, trayProp, hwnd);
	else
		Shell_NotifyIconW(NIM_MODIFY, &data);
}
#else
SDL_WindowFlags frameFlags() {
	return SDL_WINDOW_BORDERLESS;
}

void styleWindow(SDL_Window&, const Theme&, bool) {}

void notify(SDL_Window& window, bool, std::string_view, std::string_view) {
	SDL_FlashWindow(&window, SDL_FLASH_UNTIL_FOCUSED);
}
#endif

} // namespace app
