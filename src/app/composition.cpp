#include "composition.h"

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace app {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

struct Composition::State {
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGISwapChain1> source, chain;
	ComPtr<IDCompositionDevice> device;
	ComPtr<IDCompositionTarget> target;
	ComPtr<IDCompositionVisual> visual;
	UINT width = 0, height = 0;
};

Composition::Composition() = default;
Composition::~Composition() = default;

bool Composition::attach(SDL_Window& window, SDL_Renderer& renderer) {
	const auto properties = SDL_GetRendererProperties(&renderer);
	ComPtr<ID3D11Device> d3d(static_cast<ID3D11Device*>(
	    SDL_GetPointerProperty(properties, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr)));
	ComPtr<IDXGISwapChain1> source(static_cast<IDXGISwapChain1*>(
	    SDL_GetPointerProperty(properties, SDL_PROP_RENDERER_D3D11_SWAPCHAIN_POINTER, nullptr)));
	const auto hwnd = static_cast<HWND>(
	    SDL_GetPointerProperty(SDL_GetWindowProperties(&window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	if (!d3d || !source || !hwnd)
		return false;
	auto state = std::make_unique<State>();
	state->source = source;
	d3d->GetImmediateContext(&state->context);
	DXGI_SWAP_CHAIN_DESC1 sourceDesc{};
	source->GetDesc1(&sourceDesc);
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<IDXGIFactory2> factory;
	if (FAILED(d3d.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter))
	    || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
		return false;
	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = sourceDesc.Width;
	desc.Height = sourceDesc.Height;
	desc.Format = sourceDesc.Format;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	// The frame holds premultiplied colour: the canvas is cleared premultiplied and blending
	// over it keeps it so.
	desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	if (FAILED(factory->CreateSwapChainForComposition(d3d.Get(), &desc, nullptr, &state->chain))
	    || FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&state->device)))
	    || FAILED(state->device->CreateTargetForHwnd(hwnd, TRUE, &state->target))
	    || FAILED(state->device->CreateVisual(&state->visual)) || FAILED(state->visual->SetContent(state->chain.Get()))
	    || FAILED(state->target->SetRoot(state->visual.Get())) || FAILED(state->device->Commit()))
		return false;
	state->width = desc.Width;
	state->height = desc.Height;
	state_ = std::move(state);
	// SDL's own chain is presented only to keep the window's surface empty (below); the
	// composition chain carries vsync, so SDL's must not wait for a second vblank per frame.
	SDL_SetRenderVSync(&renderer, 0);
	clearSurface(renderer);
	return true;
}

void Composition::clearSurface(SDL_Renderer& renderer) {
	// The window's own surface (SDL_WINDOW_TRANSPARENT: alpha honoured) lies under the visual and
	// shows through its translucent pixels. Windows keeps showing old frames there (the system
	// caption painted at creation, a transcript drawn during a live resize), which overlapped
	// the real one; present it fully transparent after every frame.
	SDL_SetRenderDrawColor(&renderer, 0, 0, 0, 0);
	SDL_RenderClear(&renderer);
	SDL_RenderPresent(&renderer);
}

void Composition::present(SDL_Renderer& renderer) {
	auto& state = *state_;
	SDL_FlushRenderer(&renderer);
	// SDL's chain is presented only to clear it, so after drawing buffer 0 holds this frame.
	ComPtr<ID3D11Texture2D> frame, back;
	if (FAILED(state.source->GetBuffer(0, IID_PPV_ARGS(&frame))))
		return;
	D3D11_TEXTURE2D_DESC frameDesc{};
	frame->GetDesc(&frameDesc);
	if (frameDesc.Width != state.width || frameDesc.Height != state.height) {
		if (FAILED(state.chain->ResizeBuffers(0, frameDesc.Width, frameDesc.Height, DXGI_FORMAT_UNKNOWN, 0)))
			return;
		state.width = frameDesc.Width;
		state.height = frameDesc.Height;
	}
	if (FAILED(state.chain->GetBuffer(0, IID_PPV_ARGS(&back))))
		return;
	state.context->CopyResource(back.Get(), frame.Get());
	state.chain->Present(1, 0);
	clearSurface(renderer);
}

SDL_WindowFlags Composition::windowFlags() {
	return SDL_WINDOW_TRANSPARENT;
}
#else
struct Composition::State {};
Composition::Composition() = default;
Composition::~Composition() = default;
bool Composition::attach(SDL_Window&, SDL_Renderer&) {
	return true; // SDL_WINDOW_TRANSPARENT already composes with alpha here.
}
SDL_WindowFlags Composition::windowFlags() {
	return SDL_WINDOW_TRANSPARENT;
}
void Composition::present(SDL_Renderer&) {}
#endif

} // namespace app
