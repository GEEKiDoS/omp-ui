#pragma once

#include "text/inline_layout.h"

namespace kinds {

struct Preedit {
	std::string text;
	int start = 0, length = 0;
	text::Runs runs;
	text::Layout layout;
};
void setPreedit(Preedit& state, text::Engine& engine, std::string_view value, int start, int length);
void paintPreedit(const Preedit& state, text::Engine& engine, const text::Fonts& fonts, SDL_Renderer& renderer,
                  const SDL_FRect& caret);

} // namespace kinds
