#pragma once

#include "gfx/image.h"
#include "spans.h"
#include <functional>
#include <list>
#include <optional>
#include <unordered_map>

namespace text {

enum class Wrap { Word, Char, None };
enum class Truncate { End, Start, Middle };
struct LayoutOptions {
	float width = 1;
	Wrap wrap = Wrap::Word;
	Truncate truncate = Truncate::End;
	int maxLines = 0;
	std::optional<size_t> caret;
};
struct Piece {
	size_t run = 0, begin = 0, end = 0;
	float x = 0, width = 0;
	int line = 0;
	bool ellipsis = false;
};
struct Layout {
	std::vector<Piece> pieces;
	std::vector<float> lineWidths;
	float width = 0, height = 0;
	float lineHeight = 0, ascent = 0;
	bool clipped = false;
	std::optional<SDL_FPoint> caret;
};
struct Hit {
	size_t byte = 0;
	std::string_view href;
};
// Transcript selection between two output-pixel points, `from` first in reading order.
// `word`: select the word under `from` instead (double-click).
struct Selection {
	SDL_FPoint from, to;
	bool word = false;
};
struct TextDeleter {
	void operator()(TTF_Text* value) const { TTF_DestroyText(value); }
};
struct EngineDeleter {
	void operator()(TTF_TextEngine* value) const { TTF_DestroyRendererTextEngine(value); }
};

class Engine {
public:
	Engine(SDL_Renderer& renderer, Fonts& fonts);
	bool valid() const { return static_cast<bool>(engine_); }
	Layout layout(const Runs& runs, const LayoutOptions& options) const;
	void draw(const Runs& runs, const Layout& layout, float x, float y, float opacity = 1);
	Hit hit(const Runs& runs, const Layout& layout, float x, float y);
	void clear();
	// After a full paint: releases picture textures not drawn for a few seconds (scrolled away),
	// so VRAM holds what is on screen rather than every screenshot up to the budget.
	void trimPictures();
	// While set, draw() highlights the covered text and appends it to selectedText().
	void setSelection(std::optional<Selection> selection) {
		selection_ = selection;
		if (selection)
			selectedText_.clear();
	}
	const std::string& selectedText() const { return selectedText_; }
	// Draws a decoded picture into `rect`, uploading its pixels once per picture, size and renderer.
	void drawImage(const gfx::Image& image, const SDL_FRect& rect, float opacity = 1);
	// A transcript gather visits the same visible pieces without shaping or drawing glyphs.
	std::function<void(const Runs&, const Layout&, float, float, SDL_FRect)> capture;
	std::function<void(const Runs&, const Layout&, float, float, float)> decorate;
	void paintHighlight(const Runs& runs, const Layout& layout, float x, float y, size_t begin, size_t end,
	                    bool current, float opacity);

private:
	struct CacheKey {
		FontId font;
		Uint32 color;
		std::string_view value;
		bool operator==(const CacheKey&) const = default;
	};
	struct CacheHash {
		size_t operator()(const CacheKey& key) const;
	};
	struct Cached {
		std::string value;
		FontId font;
		Uint32 color;
		std::unique_ptr<TTF_Text, TextDeleter> text;
		// How far SDL_ttf moved the text down inside its box: a glyph rising above the font's ascent
		// (an emoji from a taller fallback face) shifts the whole line by its overshoot. Drawing
		// subtracts it, so every piece keeps the line's baseline.
		float top = 0;
	};
	const Cached* cached(FontId font, SDL_Color color, std::string_view value);
	void paintSelection(const Runs& runs, const Layout& layout, float x, float y, float lineHeight, float opacity);
	SDL_Renderer& renderer_;
	Fonts& fonts_;
	std::unique_ptr<TTF_TextEngine, EngineDeleter> engine_;
	std::list<Cached> cache_;
	std::unordered_map<CacheKey, std::list<Cached>::iterator, CacheHash> index_;
	std::optional<Selection> selection_;
	std::string selectedText_;
	struct TextureDeleter {
		void operator()(SDL_Texture* value) const { SDL_DestroyTexture(value); }
	};
	// One texture per picture, sized to how it is drawn; least recently drawn first to go once
	// their total passes pictureBudget, so long sessions don't keep every screenshot in VRAM.
	struct Picture {
		size_t id;
		int width, height;
		std::unique_ptr<SDL_Texture, TextureDeleter> texture;
		Uint64 drawn = 0;
		size_t bytes() const { return static_cast<size_t>(width) * height * 4; }
	};
	std::list<Picture> pictures_; // Most recently drawn first.
	size_t pictureBytes_ = 0;
};

} // namespace text
