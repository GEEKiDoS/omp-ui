#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunasvg {
class Document;
}

namespace gfx {

// Premultiplied ARGB8888 pixels (SDL_PIXELFORMAT_ARGB8888, rows packed).
struct Pixels {
	int width = 0, height = 0;
	std::vector<Uint32> argb;
};

// Default height cap for pictures in the transcript, in text lines.
inline constexpr float pictureLines = 24;

// A decoded picture. Raster pictures keep only their encoded bytes (a screenshot's pixels are
// ~4 MB; long sessions show dozens) and decode again when a texture needs them; SVG documents
// render at each display size so they stay sharp.
class Image {
public:
	// PNG, JPEG, GIF (first frame), BMP and WebP bytes, or SVG text; nullopt when undecodable.
	static std::optional<Image> decode(std::string_view bytes);
	static std::optional<Image> svg(std::string_view source);
	Image(Image&&) noexcept;
	Image& operator=(Image&&) noexcept;
	~Image();
	// Natural size in CSS pixels (scale by the display factor to place it).
	float width() const { return width_; }
	float height() const { return height_; }
	// Content identity for texture caches.
	size_t id() const { return id_; }
	bool vector() const { return document_ != nullptr; }
	// Natural size × `scale`, shrunk (aspect kept) to fit `maxWidth`×`maxHeight`.
	SDL_FPoint fitted(float scale, float maxWidth, float maxHeight) const;
	// The pixels to show at `width`×`height` output pixels: vector pictures render at that size;
	// raster ones are box-filtered down to it, or keep their natural size when it is larger.
	Pixels pixels(int width, int height) const;

private:
	Image() = default;
	float width_ = 0, height_ = 0;
	size_t id_ = 0;
	std::string encoded_;
	std::unique_ptr<lunasvg::Document> document_;
};

std::string decodeBase64(std::string_view text);

} // namespace gfx
