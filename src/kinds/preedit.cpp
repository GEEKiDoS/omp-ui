#include "preedit.h"
#include "core/text_util.h"
#include "gfx/draw.h"
#include <algorithm>

namespace kinds {
namespace {
size_t characterByte(std::string_view value, int characters) {
	size_t offset = 0;
	for (int i = 0; i < characters && offset < value.size(); ++i) {
		++offset;
		while (offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80)
			++offset;
	}
	return offset;
}
} // namespace
void setPreedit(Preedit& state, text::Engine& engine, std::string_view value, int start, int length) {
	if (state.text == value && state.start == start && state.length == length)
		return;
	state.text = value;
	state.start = start;
	state.length = length;
	state.runs.clear();
	const size_t begin = characterByte(value, std::max(0, start));
	const size_t end = characterByte(value, std::max(0, start) + std::max(0, length));
	text::RunStyle style{.color = text::palette::fg};
	style.underline = true;
	state.runs.push_back({std::string(value.substr(0, begin)), style});
	style.bg = text::palette::surface2;
	state.runs.push_back({std::string(value.substr(begin, end - begin)), style});
	style.bg = {0, 0, 0, 0};
	state.runs.push_back({std::string(value.substr(end)), style});
	state.layout =
	    engine.layout(state.runs, {.width = 1000000, .wrap = text::Wrap::None, .maxLines = 1, .caret = begin});
}
void paintPreedit(const Preedit& state, text::Engine& engine, const text::Fonts& fonts, SDL_Renderer& renderer,
                  const SDL_FRect& caret) {
	if (state.text.empty())
		return;
	const float em = fonts.size(), unit = em / 15, padX = em * gfx::space::xs;
	// The caret is inset 2 units at both ends of its line; the composition sits on the line itself.
	const float top = caret.y - unit * 2, lineHeight = caret.h + unit * 4;
	const SDL_FRect box{caret.x - padX, top, state.layout.width + padX * 2, std::max(state.layout.height, lineHeight)};
	gfx::rounded(renderer, box, unit * 3, text::palette::surface);
	engine.draw(state.runs, state.layout, caret.x, top);
	const float x = caret.x + (state.layout.caret ? state.layout.caret->x : 0);
	gfx::fill(renderer, {x, caret.y, std::max(1.0f, unit * 2), caret.h}, text::palette::accent);
}
} // namespace kinds
