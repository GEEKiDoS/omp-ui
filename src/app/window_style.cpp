#include "window_style.h"

#include "text/spans.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

namespace app {

#ifdef _WIN32
namespace {

constexpr wchar_t previousProc[] = L"omp-ui.previous-wndproc";

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
	if (message == WM_NCDESTROY)
		RemovePropW(hwnd, previousProc);
	return CallWindowProcW(previous, hwnd, message, wParam, lParam);
}

void removeFrame(HWND hwnd) {
	if (!GetPropW(hwnd, previousProc)) {
		SetPropW(hwnd, previousProc, reinterpret_cast<HANDLE>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)));
		SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(frameProc));
	}
	// A one-pixel frame inside the client keeps DWM drawing the shadow.
	const MARGINS margins{0, 0, 0, 1};
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
	// DWMWA_USE_IMMERSIVE_DARK_MODE, by number so older SDK headers still build.
	constexpr DWORD immersiveDarkMode = 20;
	const BOOL dark = text::darkPalette;
	DwmSetWindowAttribute(hwnd, immersiveDarkMode, &dark, sizeof(dark));
	removeFrame(hwnd);
	// Disabled again (Backdrop::None) when a reloaded theme drops it.
	setAccent(hwnd, translucent && theme.transparent() ? theme.backdrop : Theme::Backdrop::None);
}
#else
SDL_WindowFlags frameFlags() {
	return SDL_WINDOW_BORDERLESS;
}

void styleWindow(SDL_Window&, const Theme&, bool) {}
#endif

} // namespace app
