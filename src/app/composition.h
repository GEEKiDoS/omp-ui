#pragma once

#include <SDL3/SDL.h>

#include <memory>

namespace app {

// A translucent window's frames on Windows, presented through DirectComposition: SDL's
// Direct3D 11 swap chain is bound to the window and composes opaque, so each frame is copied
// from SDL's back buffer into a premultiplied-alpha composition swap chain shown over the
// window. Other platforms (and other renderers) need none of this: attach() returns false.
class Composition {
public:
	Composition();
	~Composition();
	Composition(const Composition&) = delete;
	Composition& operator=(const Composition&) = delete;

	// Creation flags for a window that should be translucent.
	static SDL_WindowFlags windowFlags();

	// Makes the window's frames composite with alpha: on Windows by binding a composition swap
	// chain (false, and nothing changes, on failure); elsewhere nothing is needed (true).
	bool attach(SDL_Window& window, SDL_Renderer& renderer);
	// A composition swap chain is in use: present() replaces SDL_RenderPresent.
	bool attached() const { return state_ != nullptr; }
	void present(SDL_Renderer& renderer);

private:
	static void clearSurface(SDL_Renderer& renderer);
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace app
