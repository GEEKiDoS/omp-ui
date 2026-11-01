#include "image.h"

#include "text/spans.h"

#include <lunasvg.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include <stb/stb_image.h>
#define SIMPLEWEBP_IMPLEMENTATION
#include <simplewebp/simplewebp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <functional>

namespace gfx {
namespace {

struct StbFree {
	void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
};
struct WebpFree {
	void operator()(simplewebp* webp) const { simplewebp_unload(webp); }
};

// SVG text is drawn with the bundled faces, under the names SVGs ask for and the names
// lunasvg falls back to (it scans no system fonts).
void registerFonts() {
	[[maybe_unused]] static const bool once = [] {
		const std::string base = std::string(SDL_GetBasePath()) + "fonts/";
		const auto file = [&](const char* name) {
			return base + name;
		};
		for (const char* family : {"sans-serif", "Inter", "system-ui", "Arial", "DejaVu Sans"}) {
			lunasvg_add_font_face_from_file(family, false, false, file("Inter-Regular.ttf").c_str());
			lunasvg_add_font_face_from_file(family, true, false, file("Inter-Bold.ttf").c_str());
			lunasvg_add_font_face_from_file(family, false, true, file("Inter-Italic.ttf").c_str());
		}
		for (const char* family : {"monospace", "JetBrains Mono", "Courier New", "DejaVu Sans Mono"}) {
			lunasvg_add_font_face_from_file(family, false, false, file("JetBrainsMono-Regular.ttf").c_str());
			lunasvg_add_font_face_from_file(family, true, false, file("JetBrainsMono-Bold.ttf").c_str());
		}
		return true;
	}();
}

std::string hex(SDL_Color color) {
	return std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b);
}

// Theme variables omp's SVGs use (`fill="var(--muted)"`), as our palette; `--c1`…`--c6` are
// chart series colours.
std::string themeColor(std::string_view name) {
	static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> aliases{{{"text", "fg"},
	                                                                                       {"c1", "accent"},
	                                                                                       {"c2", "success"},
	                                                                                       {"c3", "warning"},
	                                                                                       {"c4", "pending"},
	                                                                                       {"c5", "info"},
	                                                                                       {"c6", "error"}}};
	for (const auto& [alias, target] : aliases)
		if (alias == name)
			name = target;
	const auto color = text::paletteColor(name);
	return color ? hex(color->get()) : std::string{};
}

// lunasvg has no CSS custom properties: resolve `var(--name[, fallback])` and give the root
// a `color` so `currentColor` follows the theme.
std::string themed(std::string_view source) {
	std::string out;
	out.reserve(source.size());
	for (size_t at = 0; at < source.size();) {
		const auto start = source.find("var(", at);
		if (start == std::string_view::npos) {
			out.append(source.substr(at));
			break;
		}
		out.append(source.substr(at, start - at));
		size_t end = start + 4, depth = 1;
		for (; end < source.size() && depth > 0; ++end)
			depth += source[end] == '(' ? 1 : source[end] == ')' ? -1 : 0;
		auto inner = source.substr(start + 4, end - start - 5);
		const auto comma = inner.find(',');
		auto name = inner.substr(0, comma);
		while (!name.empty() && (name.front() == ' ' || name.front() == '-'))
			name.remove_prefix(1);
		while (!name.empty() && name.back() == ' ')
			name.remove_suffix(1);
		auto value = themeColor(name);
		if (value.empty() && comma != std::string_view::npos) {
			auto fallback = inner.substr(comma + 1);
			while (!fallback.empty() && fallback.front() == ' ')
				fallback.remove_prefix(1);
			value = std::string(fallback);
		}
		out += value.empty() ? hex(text::palette::fg) : value;
		at = end;
	}
	if (const auto root = out.find("<svg"); root != std::string::npos)
		out.insert(root + 4, " color=\"" + hex(text::palette::fg) + "\"");
	return out;
}

} // namespace

Image::Image(Image&&) noexcept = default;
Image& Image::operator=(Image&&) noexcept = default;
Image::~Image() = default;

std::optional<Image> Image::svg(std::string_view source) {
	registerFonts();
	auto document = lunasvg::Document::loadFromData(themed(source));
	if (!document || document->width() <= 0 || document->height() <= 0)
		return std::nullopt;
	Image image;
	image.width_ = document->width();
	image.height_ = document->height();
	image.id_ = std::hash<std::string_view>{}(source);
	image.document_ = std::move(document);
	return image;
}

namespace {

// Premultiplied ARGB pixels at the picture's natural size; empty when undecodable.
Pixels decodeRaster(std::string_view bytes) {
	Pixels result;
	const auto store = [&](const Uint8* rgba, int width, int height) {
		result = {width, height, std::vector<Uint32>(static_cast<size_t>(width) * height)};
		for (auto& pixel : result.argb) {
			const Uint32 alpha = rgba[3];
			const auto premultiply = [alpha](Uint32 channel) {
				return (channel * alpha + 127) / 255;
			};
			pixel = (alpha << 24) | (premultiply(rgba[0]) << 16) | (premultiply(rgba[1]) << 8) | premultiply(rgba[2]);
			rgba += 4;
		}
	};
	if (bytes.size() > 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") {
		// omp re-encodes tool images as WebP. The decoder only reads `data`.
		simplewebp* loaded = nullptr;
		if (simplewebp_load_from_memory(const_cast<char*>(bytes.data()), bytes.size(), nullptr, &loaded)
		    != SIMPLEWEBP_NO_ERROR)
			return result;
		const std::unique_ptr<simplewebp, WebpFree> webp(loaded);
		size_t width = 0, height = 0;
		simplewebp_get_dimensions(webp.get(), &width, &height);
		std::vector<Uint8> rgba(width * height * 4);
		if (!rgba.empty() && simplewebp_decode(webp.get(), rgba.data(), nullptr) == SIMPLEWEBP_NO_ERROR)
			store(rgba.data(), static_cast<int>(width), static_cast<int>(height));
		return result;
	}
	int width = 0, height = 0, channels = 0;
	const std::unique_ptr<stbi_uc, StbFree> rgba(stbi_load_from_memory(
	    reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &width, &height, &channels, 4));
	if (rgba)
		store(rgba.get(), width, height);
	return result;
}

// Area-average (box filter) shrink of premultiplied pixels, so downscaled screenshots keep
// their text legible instead of aliasing as a GPU bilinear minification would.
Pixels shrink(const Pixels& source, int width, int height) {
	Pixels result{width, height, std::vector<Uint32>(static_cast<size_t>(width) * height)};
	for (int y = 0; y < height; ++y) {
		const int y0 = y * source.height / height, y1 = std::max(y0 + 1, (y + 1) * source.height / height);
		for (int x = 0; x < width; ++x) {
			const int x0 = x * source.width / width, x1 = std::max(x0 + 1, (x + 1) * source.width / width);
			Uint32 a = 0, r = 0, g = 0, b = 0;
			for (int sy = y0; sy < y1; ++sy)
				for (int sx = x0; sx < x1; ++sx) {
					const Uint32 pixel = source.argb[static_cast<size_t>(sy) * source.width + sx];
					a += pixel >> 24;
					r += (pixel >> 16) & 0xff;
					g += (pixel >> 8) & 0xff;
					b += pixel & 0xff;
				}
			const Uint32 count = static_cast<Uint32>((y1 - y0) * (x1 - x0)), half = count / 2;
			result.argb[static_cast<size_t>(y) * width + x] = ((a + half) / count) << 24 | ((r + half) / count) << 16
			                                                | ((g + half) / count) << 8 | (b + half) / count;
		}
	}
	return result;
}

} // namespace

std::optional<Image> Image::decode(std::string_view bytes) {
	const auto start = bytes.find_first_not_of(" \t\r\n");
	if (start != std::string_view::npos && bytes[start] == '<')
		return svg(bytes);
	// Decoded once to validate and size it; the pixels are dropped until a texture needs them.
	const auto pixels = decodeRaster(bytes);
	if (pixels.argb.empty())
		return std::nullopt;
	Image image;
	image.width_ = static_cast<float>(pixels.width);
	image.height_ = static_cast<float>(pixels.height);
	image.id_ = std::hash<std::string_view>{}(bytes);
	image.encoded_ = bytes;
	return image;
}

SDL_FPoint Image::fitted(float scale, float maxWidth, float maxHeight) const {
	const float shrink = std::min({scale, std::max(0.0f, maxWidth) / width_, std::max(0.0f, maxHeight) / height_});
	return {width_ * shrink, height_ * shrink};
}

Pixels Image::pixels(int width, int height) const {
	if (!document_) {
		auto natural = decodeRaster(encoded_);
		if (natural.argb.empty() || (width >= natural.width && height >= natural.height))
			return natural;
		return shrink(natural, std::min(width, natural.width), std::min(height, natural.height));
	}
	auto bitmap = document_->renderToBitmap(width, height);
	Pixels result{bitmap.width(), bitmap.height(), {}};
	result.argb.resize(static_cast<size_t>(result.width) * result.height);
	for (int row = 0; row < result.height; ++row)
		SDL_memcpy(result.argb.data() + static_cast<size_t>(row) * result.width,
		           bitmap.data() + static_cast<size_t>(row) * bitmap.stride(), static_cast<size_t>(result.width) * 4);
	return result;
}

std::string decodeBase64(std::string_view text) {
	static constexpr auto table = [] {
		std::array<int, 256> values{};
		values.fill(-1);
		constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		for (size_t i = 0; i < alphabet.size(); ++i)
			values[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
		return values;
	}();
	std::string out;
	out.reserve(text.size() / 4 * 3);
	Uint32 buffer = 0;
	int bits = 0;
	for (const char ch : text) {
		const int value = table[static_cast<unsigned char>(ch)];
		if (value < 0)
			continue; // Padding and line breaks.
		buffer = (buffer << 6) | static_cast<Uint32>(value);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<char>((buffer >> bits) & 0xff));
		}
	}
	return out;
}

} // namespace gfx
