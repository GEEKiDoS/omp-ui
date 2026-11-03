#pragma once

#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace text {

enum class FontId { Ui, Bold, Italic, BoldItalic, Mono, MonoBold, MonoItalic, MonoBoldItalic, Icons, Wordmark, Count };
struct FontDeleter {
	void operator()(TTF_Font* font) const { TTF_CloseFont(font); }
};
using Font = std::unique_ptr<TTF_Font, FontDeleter>;

class Fonts {
public:
	bool load(float displayScale, std::string& error);
	void setScale(float displayScale);
	TTF_Font& get(FontId id) const {
		return id == FontId::Icons ? *fallback_.front() : *fonts_[static_cast<size_t>(id)];
	}
	float width(FontId id, std::string_view value) const;
	size_t fit(FontId id, std::string_view value, float width) const;
	float size() const { return size_; }
	float lineHeight() const { return static_cast<float>(TTF_GetFontLineSkip(&get(FontId::Ui))); }
	float monoAdvance() const { return width(FontId::Mono, "M"); }
	// How far a colour emoji (from a fallback face) rises above `id`'s own glyph box: a line
	// holding one needs that much more room above its baseline.
	float fallbackRise(FontId id) const { return rises_[static_cast<size_t>(id)]; }

private:
	// Shaped widths of short strings (words): layout asks for the same tokens over and over.
	struct WidthHash {
		using is_transparent = void;
		size_t operator()(std::string_view value) const { return std::hash<std::string_view>{}(value); }
	};
	using WidthCache = std::unordered_map<std::string, float, WidthHash, std::equal_to<>>;
	// Base faces must die before the fallback faces they reference. `fallback_` holds the system
	// faces as loaded, icons and colour emoji first; `styled_[style - 1]` copies of the text faces
	// after them, set to synthesize bold, italic, or both.
	std::vector<Font> fallback_;
	std::array<std::vector<Font>, 3> styled_;
	std::array<Font, static_cast<size_t>(FontId::Count)> fonts_;
	mutable std::array<WidthCache, static_cast<size_t>(FontId::Count)> widths_;
	float size_ = 15;
	std::array<float, static_cast<size_t>(FontId::Count)> rises_{};
	void measureRises();
};

} // namespace text
