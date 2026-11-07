#include "pointer.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace view {
namespace {
bool contains(const SDL_FRect& rect, float x, float y) {
	return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}
std::unique_ptr<SDL_Texture, ViewerTextureDeleter> imageTexture(const gfx::Image& image, SDL_Renderer& renderer) {
	const auto limit = static_cast<float>(SDL_GetNumberProperty(SDL_GetRendererProperties(&renderer),
	                                                          SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 16384));
	const float ratio = std::min({1.0f, limit / image.width(), limit / image.height()});
	const auto pixels = image.pixels(std::max(1, static_cast<int>(std::lround(image.width() * ratio))),
	                                 std::max(1, static_cast<int>(std::lround(image.height() * ratio))));
	if (pixels.argb.empty())
		return {};
	std::unique_ptr<SDL_Texture, ViewerTextureDeleter> texture(SDL_CreateTexture(
	    &renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, pixels.width, pixels.height));
	if (!texture || !SDL_UpdateTexture(texture.get(), nullptr, pixels.argb.data(), pixels.width * 4))
		return {};
	SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
	return texture;
}
} // namespace

void Views::openImageViewer(std::shared_ptr<const gfx::Image> image) {
	auto texture = imageTexture(*image, renderer_.get());
	if (!texture) {
		SDL_Log("Cannot open image viewer: %s", SDL_GetError());
		return;
	}
	dismissHover();
	menu_.clear();
	search_.focused = false; // Typing still belongs to omp while its image is on screen.
	search_.preedit({}, 0, 0);
	imageViewer_ = std::make_unique<ImageViewer>();
	imageViewer_->image = std::move(image);
	imageViewer_->texture = std::move(texture);
	imageViewer_->reveal.to(1, SDL_GetTicks(), 140);
	sizeImageViewer();
	imageViewerCursor(pointer_->position.x, pointer_->position.y);
}
void Views::closeImageViewer() {
	if (!imageViewer_)
		return;
	if (imageViewer_->press != ImageViewer::Press::None && systemPointer_)
		SDL_CaptureMouse(false);
	imageViewer_.reset();
	pointer_->suppressed = true;
	motion(pointer_->position.x, pointer_->position.y, true);
}
void Views::sizeImageViewer() {
	auto& viewer = *imageViewer_;
	int width = 1, height = 1;
	SDL_GetRenderOutputSize(&renderer_.get(), &width, &height);
	if (viewer.viewport.w == width && viewer.viewport.h == height)
		return;
	viewer.viewport = {0, 0, static_cast<float>(width), static_cast<float>(height)};
	const float margin = fonts_.size() * 2;
	viewer.fit = std::min({1.0f, std::max(1.0f, width - margin * 2) / viewer.image->width(),
	                       std::max(1.0f, height - margin * 2) / viewer.image->height()});
	if (viewer.fitted) {
		viewer.scale = viewer.fit;
		viewer.offset = {};
	} else
		viewer.scale = std::clamp(viewer.scale, viewer.fit * 0.5f, 16.0f);
	clampImageViewer();
	if (viewer.press != ImageViewer::Press::None) {
		viewer.pressAt = pointer_->position;
		viewer.pressOffset = viewer.offset;
		viewer.moved = true; // Resizing during a press is not a backdrop click.
	}
}
void Views::clampImageViewer() {
	auto& viewer = *imageViewer_;
	// Large pictures always cover the viewport on their long axes; smaller ones stay wholly
	// inside it. The latter may move while zooming so the cursor's image point stays anchored.
	const float x = std::abs(viewer.image->width() * viewer.scale - viewer.viewport.w) * 0.5f;
	const float y = std::abs(viewer.image->height() * viewer.scale - viewer.viewport.h) * 0.5f;
	viewer.offset.x = std::clamp(viewer.offset.x, -x, x);
	viewer.offset.y = std::clamp(viewer.offset.y, -y, y);
}
SDL_FRect Views::imageViewerRect() const {
	const auto& viewer = *imageViewer_;
	const float width = viewer.image->width() * viewer.scale, height = viewer.image->height() * viewer.scale;
	return {(viewer.viewport.w - width) * 0.5f + viewer.offset.x,
	        (viewer.viewport.h - height) * 0.5f + viewer.offset.y, width, height};
}
SDL_FRect Views::imageViewerCloseRect() const {
	const float em = fonts_.size();
	return {imageViewer_->viewport.w - em * 3.5f, em, em * 2.5f, em * 2.5f};
}
void Views::zoomImageViewer(float scale, float x, float y) {
	auto& viewer = *imageViewer_;
	const float next = std::clamp(scale, viewer.fit * 0.5f, 16.0f), ratio = next / viewer.scale;
	viewer.offset.x = x - viewer.viewport.w * 0.5f - (x - viewer.viewport.w * 0.5f - viewer.offset.x) * ratio;
	viewer.offset.y = y - viewer.viewport.h * 0.5f - (y - viewer.viewport.h * 0.5f - viewer.offset.y) * ratio;
	viewer.scale = next;
	viewer.fitted = false;
	clampImageViewer();
	if (viewer.press != ImageViewer::Press::None) {
		viewer.pressAt = pointer_->position;
		viewer.pressOffset = viewer.offset;
		viewer.moved = true;
	}
}
void Views::imageViewerCursor(float x, float y) {
	const auto& viewer = *imageViewer_;
	const auto cursor = contains(imageViewerCloseRect(), x, y) ? PointerState::Cursor::Hand
	                  : viewer.press == ImageViewer::Press::Image ? PointerState::Cursor::Grab
	                  : contains(imageViewerRect(), x, y) ? PointerState::Cursor::Hand
	                                                       : PointerState::Cursor::Arrow;
	if (pointer_->cursor == cursor)
		return;
	pointer_->cursor = cursor;
	if (systemPointer_)
		SDL_SetCursor(cursor == PointerState::Cursor::Grab && pointer_->grab ? pointer_->grab.get()
		              : cursor != PointerState::Cursor::Arrow ? pointer_->pointer.get()
		                                                       : pointer_->arrow.get());
}
bool Views::handleImageViewer(const SDL_Event& event, float sx, float sy) {
	sizeImageViewer();
	auto& viewer = *imageViewer_;
	switch (event.type) {
	case SDL_EVENT_MOUSE_MOTION: {
		const float x = event.motion.x * sx, y = event.motion.y * sy;
		pointer_->position = {x, y};
		if (viewer.press != ImageViewer::Press::None) {
			const float dx = x - viewer.pressAt.x, dy = y - viewer.pressAt.y;
			viewer.moved |= std::abs(dx) + std::abs(dy) > fonts_.size() * 0.3f;
			if (viewer.press == ImageViewer::Press::Image && viewer.moved) {
				viewer.offset = {viewer.pressOffset.x + dx, viewer.pressOffset.y + dy};
				viewer.fitted = false;
				clampImageViewer();
			}
		}
		imageViewerCursor(x, y);
		return true;
	}
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		pointer_->viewerButtons |= SDL_BUTTON_MASK(event.button.button);
		if (event.button.button != SDL_BUTTON_LEFT)
			return true;
		const float x = event.button.x * sx, y = event.button.y * sy;
		pointer_->position = {x, y};
		viewer.pressAt = {x, y};
		viewer.pressOffset = viewer.offset;
		viewer.moved = false;
		viewer.press = contains(imageViewerCloseRect(), x, y) ? ImageViewer::Press::Close
		             : contains(imageViewerRect(), x, y) ? ImageViewer::Press::Image
		                                                 : ImageViewer::Press::Backdrop;
		imageViewerCursor(x, y);
		if (systemPointer_)
			SDL_CaptureMouse(true);
		return true;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP: {
		pointer_->viewerButtons &= ~SDL_BUTTON_MASK(event.button.button);
		if (event.button.button != SDL_BUTTON_LEFT)
			return true;
		const float x = event.button.x * sx, y = event.button.y * sy;
		pointer_->position = {x, y};
		const auto press = viewer.press;
		viewer.press = ImageViewer::Press::None;
		if (systemPointer_)
			SDL_CaptureMouse(false);
		const bool click = !viewer.moved
		                && std::abs(x - viewer.pressAt.x) + std::abs(y - viewer.pressAt.y) <= fonts_.size() * 0.3f;
		if (click && ((press == ImageViewer::Press::Backdrop && !contains(imageViewerRect(), x, y))
		              || (press == ImageViewer::Press::Close && contains(imageViewerCloseRect(), x, y)))) {
			closeImageViewer();
			return true;
		}
		if (click && press == ImageViewer::Press::Image && event.button.clicks >= 2) {
			const bool fit = std::abs(viewer.scale - 1) < 0.001f;
			zoomImageViewer(fit ? viewer.fit : 1, x, y);
			viewer.fitted = fit;
		}
		imageViewerCursor(x, y);
		return true;
	}
	case SDL_EVENT_MOUSE_WHEEL: {
		const float lines = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
		zoomImageViewer(viewer.scale * std::pow(1.15f, lines), event.wheel.mouse_x * sx, event.wheel.mouse_y * sy);
		return true;
	}
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		viewer.press = ImageViewer::Press::None;
		if (systemPointer_)
			SDL_CaptureMouse(false);
		return true;
	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		return true;
	default:
		return false; // Only Esc is claimed through the existing local-key callbacks.
	}
}
bool Views::paintImageViewer() {
	if (!imageViewer_)
		return false;
	sizeImageViewer();
	auto& viewer = *imageViewer_;
	auto& renderer = renderer_.get();
	if (!viewer.texture) {
		viewer.texture = imageTexture(*viewer.image, renderer);
		if (!viewer.texture) {
			SDL_Log("Cannot restore image viewer: %s", SDL_GetError());
			closeImageViewer();
			return false;
		}
	}
	const float progress = viewer.reveal.value(SDL_GetTicks()), em = fonts_.size();
	gfx::fill(renderer, viewer.viewport, text::rgb(0, static_cast<Uint8>(std::lround(175 * progress))));
	auto rect = imageViewerRect();
	const float grow = 0.97f + progress * 0.03f;
	rect.x += rect.w * (1 - grow) * 0.5f;
	rect.y += rect.h * (1 - grow) * 0.5f;
	rect.w *= grow;
	rect.h *= grow;
	SDL_SetTextureScaleMode(viewer.texture.get(), viewer.scale > 1 ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
	SDL_SetTextureColorModFloat(viewer.texture.get(), progress, progress, progress);
	SDL_SetTextureAlphaModFloat(viewer.texture.get(), progress);
	SDL_RenderTexture(&renderer, viewer.texture.get(), nullptr, &rect);
	const auto close = imageViewerCloseRect();
	const bool hovered = contains(close, pointer_->position.x, pointer_->position.y);
	gfx::rounded(renderer, close, em * 0.45f, hovered ? text::palette::hover : text::palette::surface2);
	gfx::roundedBorder(renderer, close, em * 0.45f, text::palette::border);
	const auto color = text::palette::fg;
	gfx::line(renderer, {close.x + em * 0.85f, close.y + em * 0.85f},
	          {close.x + em * 1.65f, close.y + em * 1.65f}, color, em / 10);
	gfx::line(renderer, {close.x + em * 1.65f, close.y + em * 0.85f},
	          {close.x + em * 0.85f, close.y + em * 1.65f}, color, em / 10);
	const int percent = static_cast<int>(std::lround(viewer.scale * 100));
	if (viewer.percent != percent || viewer.labelEm != em) {
		viewer.percent = percent;
		viewer.labelEm = em;
		viewer.label = {{std::format("{}%", percent), {.color = text::palette::fg}}};
		viewer.labelLayout = engine_.get().layout(viewer.label, {.width = em * 8, .wrap = text::Wrap::None});
	}
	const SDL_FRect badge{em, viewer.viewport.h - viewer.labelLayout.height - em * 2,
	                      viewer.labelLayout.width + em * 1.2f, viewer.labelLayout.height + em};
	gfx::rounded(renderer, badge, em * 0.35f, text::palette::surface2);
	engine_.get().draw(viewer.label, viewer.labelLayout, badge.x + em * 0.6f, badge.y + em * 0.5f);
	return viewer.reveal.active(SDL_GetTicks());
}

} // namespace view
