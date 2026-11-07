#pragma once

#include "view_tree.h"

namespace view {

struct CursorDeleter {
	void operator()(SDL_Cursor* cursor) const { SDL_DestroyCursor(cursor); }
};
struct ViewerTextureDeleter {
	void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};
struct ImageViewer {
	std::shared_ptr<const gfx::Image> image;
	// Separate from the transcript's size-keyed cache: zoom only scales this one upload.
	std::unique_ptr<SDL_Texture, ViewerTextureDeleter> texture;
	SDL_FRect viewport{};
	SDL_FPoint offset{}, pressAt{}, pressOffset{};
	float scale = 1, fit = 1;
	bool fitted = true, moved = false;
	enum class Press { None, Image, Backdrop, Close };
	Press press = Press::None;
	gfx::Tween reveal;
	text::Runs label;
	text::Layout labelLayout;
	int percent = -1;
	float labelEm = 0;
};
struct PointerState {
	struct NumberDrag {
		std::string surface, node, item;
		json control;
		SDL_FRect track{};
		float startX = 0;
	};
	std::optional<NumberDrag> number;
	struct ImagePress {
		std::shared_ptr<const gfx::Image> image;
		SDL_FPoint point;
		bool moved = false;
	};
	std::optional<ImagePress> imagePress;
	Uint32 viewerButtons = 0;
	std::string previewItem;
	std::string surface, node, tip;
	Uint64 since = 0, lastCard = 0;
	enum class Cursor { Arrow, Hand, Resize, Grab };
	Cursor cursor = Cursor::Arrow;
	bool shown = false, passThrough = false, suppressed = false;
	bool cardPress = false;
	SDL_FPoint position{-1, -1};
	// Where the pointer was when the hover appeared: it opens there, as system tooltips do.
	std::optional<SDL_FPoint> shownAt;
	SDL_FRect cardRect{};
	gfx::Tween reveal;
	std::optional<SDL_FRect> tipRect;
	text::Runs title;
	text::Layout titleLayout;
	tsp::Surface cardSurface;
	std::unique_ptr<Tree> card;
	std::unique_ptr<SDL_Cursor, CursorDeleter> arrow{SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT)};
	std::unique_ptr<SDL_Cursor, CursorDeleter> pointer{SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER)};
	std::unique_ptr<SDL_Cursor, CursorDeleter> resize{SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE)};
	std::unique_ptr<SDL_Cursor, CursorDeleter> grab{SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE)};
};

} // namespace view
