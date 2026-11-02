#pragma once

#include "view_tree.h"

namespace view {

struct CursorDeleter {
	void operator()(SDL_Cursor* cursor) const { SDL_DestroyCursor(cursor); }
};
struct PointerState {
	struct NumberDrag {
		std::string surface, node, item;
		json control;
		SDL_FRect track{};
		float startX = 0;
	};
	std::optional<NumberDrag> number;
	std::string previewItem;
	std::string surface, node, tip;
	Uint64 since = 0, lastCard = 0;
	enum class Cursor { Arrow, Hand, Resize };
	Cursor cursor = Cursor::Arrow;
	bool shown = false, passThrough = false, suppressed = false;
	bool cardPress = false;
	SDL_FPoint position{-1, -1};
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
};

} // namespace view
