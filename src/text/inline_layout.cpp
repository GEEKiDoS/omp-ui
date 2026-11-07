#include "inline_layout.h"

#include "gfx/draw.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace text {
namespace {

constexpr std::string_view ellipsisText = "\xe2\x80\xa6";
constexpr size_t cacheLimit = 512;
constexpr int atlasSize = 256;

enum class TokenKind { Text, Space, Tab, Newline, Skip };
struct Token {
	size_t run, begin, end;
	TokenKind kind;
};
struct Line {
	std::vector<Piece> pieces;
	float width = 0;
	// Unhinted glyphs have fractional advances, but shaped widths come back as whole pixels: a piece
	// grown from separately measured tokens is short by up to a pixel per token. `slack` bounds that
	// error; the line is measured again as whole pieces when it matters (see settle in layout()).
	float slack = 0;
};

size_t boundaryBefore(std::string_view value, size_t offset) {
	offset = std::min(offset, value.size());
	while (offset > 0 && offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80)
		--offset;
	return offset;
}

size_t boundaryAfter(std::string_view value, size_t offset) {
	if (offset == value.size())
		return offset;
	++offset;
	while (offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80)
		++offset;
	return offset;
}

// Colour emoji, which only a fallback face draws: the supplementary pictographs (4-byte UTF-8,
// U+1F000 and up) and the BMP's Miscellaneous Symbols and Dingbats (U+2600–U+27BF).
bool hasEmoji(std::string_view value) {
	for (size_t i = 0; i < value.size(); ++i) {
		const auto lead = static_cast<unsigned char>(value[i]);
		if (lead >= 0xF0)
			return true;
		if (lead == 0xE2 && i + 1 < value.size()) {
			const auto next = static_cast<unsigned char>(value[i + 1]);
			if (next >= 0x98 && next <= 0x9E) // U+2600–U+27BF
				return true;
		}
	}
	return false;
}

FontId fontFor(const RunStyle& style) {
	return style.icon ? FontId::Icons : style.font;
}

FontId fontFor(const RunStyle& style, const Piece& piece) {
	return piece.ellipsis ? FontId::Ui : fontFor(style);
}

std::string_view valueFor(const Runs& runs, const Piece& piece) {
	return piece.ellipsis ? ellipsisText
	                      : std::string_view(runs[piece.run].text).substr(piece.begin, piece.end - piece.begin);
}

bool controlPiece(const Runs& runs, const Piece& piece) {
	if (piece.ellipsis || piece.begin == piece.end)
		return false;
	const char first = runs[piece.run].text[piece.begin];
	return first == '\t' || first == '\n' || first == '\r';
}

bool newlinePiece(const Runs& runs, const Piece& piece) {
	if (piece.ellipsis || piece.begin == piece.end)
		return false;
	const char first = runs[piece.run].text[piece.begin];
	return first == '\n' || first == '\r';
}

float iconGap(const Runs& runs, const Fonts& fonts, size_t run, size_t end) {
	if (!runs[run].style.icon || end != runs[run].text.size())
		return 0;
	for (size_t next = run + 1; next < runs.size(); ++next) {
		if (runs[next].text.empty())
			continue;
		const char first = runs[next].text.front();
		return !runs[next].style.icon && first != '\n' && first != '\r' ? fonts.size() * gfx::space::sm : 0;
	}
	return 0;
}

float codePadding(const RunStyle& style, const Fonts& fonts) {
	return style.inlineCode ? fonts.size() * gfx::space::xs : 0;
}

float rangeWidth(const Runs& runs, const Fonts& fonts, size_t run, size_t begin, size_t end) {
	if (begin == end)
		return 0;
	return fonts.width(fontFor(runs[run].style), std::string_view(runs[run].text).substr(begin, end - begin))
	     + codePadding(runs[run].style, fonts) * 2 + iconGap(runs, fonts, run, end);
}

void position(Line& line) {
	line.width = 0;
	for (auto& piece : line.pieces) {
		piece.x = line.width;
		line.width += piece.width;
	}
}

std::vector<Token> tokenize(const Runs& runs) {
	std::vector<Token> tokens;
	tokens.reserve(runs.size());
	bool afterCr = false;
	for (size_t run = 0; run < runs.size(); ++run) {
		const auto& value = runs[run].text;
		for (size_t begin = 0; begin < value.size();) {
			const char first = value[begin];
			TokenKind kind = TokenKind::Text;
			if (first == '\n')
				kind = afterCr ? TokenKind::Skip : TokenKind::Newline;
			else if (first == '\r')
				kind = TokenKind::Newline;
			else if (first == '\t')
				kind = TokenKind::Tab;
			else if (first == ' ')
				kind = TokenKind::Space;
			afterCr = first == '\r';
			size_t end = begin + 1;
			if (kind == TokenKind::Text || kind == TokenKind::Space) {
				while (end < value.size()) {
					const char next = value[end];
					if (next == '\n' || next == '\r' || next == '\t' || (next == ' ') != (kind == TokenKind::Space))
						break;
					++end;
				}
			}
			tokens.push_back({run, begin, end, kind});
			begin = end;
		}
	}
	return tokens;
}

Line prefix(const Runs& runs, const Fonts& fonts, const Line& source, float budget) {
	Line result;
	for (auto piece : source.pieces) {
		const float remaining = std::max(0.0f, budget - result.width);
		if (piece.width > remaining) {
			if (controlPiece(runs, piece)) {
				piece.width = remaining;
			} else {
				const auto value = std::string_view(runs[piece.run].text);
				const float textBudget = std::max(0.0f, remaining - codePadding(runs[piece.run].style, fonts) * 2);
				const size_t length = fonts.fit(fontFor(runs[piece.run].style),
				                                value.substr(piece.begin, piece.end - piece.begin), textBudget);
				piece.end = boundaryBefore(value, piece.begin + std::min(length, piece.end - piece.begin));
				piece.width = rangeWidth(runs, fonts, piece.run, piece.begin, piece.end);
				while (piece.end > piece.begin && piece.width > remaining) {
					piece.end = boundaryBefore(value, piece.end - 1);
					piece.width = rangeWidth(runs, fonts, piece.run, piece.begin, piece.end);
				}
			}
			if (piece.end > piece.begin && (piece.width > 0 || !controlPiece(runs, piece))) {
				piece.x = result.width;
				result.width += piece.width;
				result.pieces.push_back(piece);
			}
			break;
		}
		piece.x = result.width;
		result.width += piece.width;
		result.pieces.push_back(piece);
	}
	return result;
}

Line suffix(const Runs& runs, const Fonts& fonts, const Line& source, float budget) {
	Line result;
	for (auto it = source.pieces.rbegin(); it != source.pieces.rend(); ++it) {
		auto piece = *it;
		const float remaining = std::max(0.0f, budget - result.width);
		if (piece.width > remaining) {
			if (controlPiece(runs, piece)) {
				piece.width = remaining;
			} else {
				const auto value = std::string_view(runs[piece.run].text);
				std::vector<size_t> boundaries;
				boundaries.push_back(piece.begin);
				for (size_t at = piece.begin; at < piece.end;) {
					at = boundaryAfter(value, at);
					boundaries.push_back(at);
				}
				size_t low = 0, high = boundaries.size() - 1;
				while (low < high) {
					const size_t middle = low + (high - low) / 2;
					if (rangeWidth(runs, fonts, piece.run, boundaries[middle], piece.end) <= remaining)
						high = middle;
					else
						low = middle + 1;
				}
				piece.begin = boundaries[low];
				piece.width = rangeWidth(runs, fonts, piece.run, piece.begin, piece.end);
			}
			if (piece.end > piece.begin && (piece.width > 0 || !controlPiece(runs, piece))) {
				result.width += piece.width;
				result.pieces.push_back(piece);
			}
			break;
		}
		result.width += piece.width;
		result.pieces.push_back(piece);
	}
	std::reverse(result.pieces.begin(), result.pieces.end());
	position(result);
	return result;
}

Line truncateLine(const Runs& runs, const Fonts& fonts, const Line& front, const Line& back, float width,
                  Truncate mode) {
	const float measuredEllipsis = fonts.width(FontId::Ui, ellipsisText);
	const float ellipsisWidth = measuredEllipsis <= width ? measuredEllipsis : 0;
	const float budget = width - ellipsisWidth;
	Line left, right;
	if (mode == Truncate::End)
		left = prefix(runs, fonts, front, budget);
	else if (mode == Truncate::Start)
		right = suffix(runs, fonts, back, budget);
	else {
		left = prefix(runs, fonts, front, budget * 0.5f);
		right = suffix(runs, fonts, back, budget - left.width);
	}
	Piece marker;
	if (!left.pieces.empty()) {
		marker = left.pieces.back();
		marker.begin = marker.end;
	} else if (!right.pieces.empty()) {
		marker = right.pieces.front();
		marker.end = marker.begin;
	} else if (mode == Truncate::Start) {
		marker = back.pieces.back();
		marker.begin = marker.end;
	} else {
		marker = front.pieces.front();
		marker.end = marker.begin;
	}
	marker.width = ellipsisWidth;
	marker.ellipsis = true;
	if (ellipsisWidth > 0)
		left.pieces.push_back(marker);
	left.pieces.insert(left.pieces.end(), right.pieces.begin(), right.pieces.end());
	position(left);
	return left;
}

Uint32 colorKey(SDL_Color color) {
	return (static_cast<Uint32>(color.r) << 24) | (static_cast<Uint32>(color.g) << 16)
	     | (static_cast<Uint32>(color.b) << 8) | color.a;
}

size_t globalByte(const Runs& runs, size_t run, size_t offset) {
	for (size_t index = 0; index < run; ++index)
		offset += runs[index].text.size();
	return offset;
}

void setColor(SDL_Renderer& renderer, SDL_Color color) {
	SDL_SetRenderDrawColor(&renderer, color.r, color.g, color.b, color.a);
}

void rule(SDL_Renderer& renderer, float x, float y, float width, float thickness) {
	const SDL_FRect rect{x, y, width, thickness};
	SDL_RenderFillRect(&renderer, &rect);
}

void wave(SDL_Renderer& renderer, float x, float y, float width, float size) {
	const float step = std::max(2.0f, size * 0.15f);
	const float amplitude = std::max(1.0f, size * 0.06f);
	std::array<SDL_FPoint, 64> points;
	size_t count = 0, vertex = 0;
	float at = 0;
	for (;;) {
		points[count++] = {x + at, y + (vertex % 2 == 0 ? 0 : amplitude)};
		if (at == width)
			break;
		if (count == points.size()) {
			SDL_RenderLines(&renderer, points.data(), static_cast<int>(count));
			points[0] = points[count - 1];
			count = 1;
		}
		at = std::min(width, at + step);
		++vertex;
	}
	if (count > 1)
		SDL_RenderLines(&renderer, points.data(), static_cast<int>(count));
}

TTF_TextEngine* createEngine(SDL_Renderer& renderer) {
	// Every font face/size gets its own atlas pages; the 1024² default leaves most of each page empty.
	const SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, TTF_PROP_RENDERER_TEXT_ENGINE_RENDERER, &renderer);
	SDL_SetNumberProperty(props, TTF_PROP_RENDERER_TEXT_ENGINE_ATLAS_TEXTURE_SIZE, atlasSize);
	TTF_TextEngine* engine = TTF_CreateRendererTextEngineWithProperties(props);
	SDL_DestroyProperties(props);
	return engine;
}

} // namespace

Engine::Engine(SDL_Renderer& renderer, Fonts& fonts)
    : renderer_(renderer), fonts_(fonts), engine_(createEngine(renderer)) {
	index_.reserve(cacheLimit);
}

Layout Engine::layout(const Runs& runs, const LayoutOptions& options) const {
	const float width = std::max(0.0f, options.width);
	Layout result;
	result.lineHeight = fonts_.lineHeight();
	result.ascent = static_cast<float>(TTF_GetFontAscent(&fonts_.get(FontId::Ui)));
	std::array<bool, static_cast<size_t>(FontId::Count)> measuredFonts{};
	measuredFonts[static_cast<size_t>(FontId::Ui)] = true;
	std::vector<size_t> bases;
	bases.reserve(runs.size() + 1);
	bases.push_back(0);
	bool hasInlineCode = false;
	float rise = 0;
	for (const auto& run : runs) {
		bases.push_back(bases.back() + run.text.size());
		hasInlineCode |= run.style.inlineCode && !run.text.empty();
		const auto fontId = fontFor(run.style);
		const auto index = static_cast<size_t>(fontId);
		if (!measuredFonts[index]) {
			auto& font = fonts_.get(fontId);
			result.lineHeight = std::max(result.lineHeight, static_cast<float>(TTF_GetFontLineSkip(&font)));
			result.ascent = std::max(result.ascent, static_cast<float>(TTF_GetFontAscent(&font)));
			measuredFonts[index] = true;
		}
		// An emoji (drawn from a taller fallback face) needs room above the line's other text.
		if (fonts_.fallbackRise(fontId) > rise && hasEmoji(run.text))
			rise = fonts_.fallbackRise(fontId);
	}
	result.lineHeight += rise;
	result.ascent += rise;
	if (hasInlineCode) {
		const float padding = fonts_.size() * gfx::space::xs;
		result.lineHeight += padding * 2;
		result.ascent += padding;
	}
	const auto tokens = tokenize(runs);
	// Shape each token once: re-measuring the growing line per word is quadratic in paragraph length.
	std::vector<float> tokenWidths(tokens.size());
	for (size_t index = 0; index < tokens.size(); ++index) {
		const auto& token = tokens[index];
		if (token.kind == TokenKind::Text || token.kind == TokenKind::Space)
			tokenWidths[index] =
			    fonts_.width(fontFor(runs[token.run].style),
			                 std::string_view(runs[token.run].text).substr(token.begin, token.end - token.begin));
	}
	std::vector<Line> lines(1);
	if (!runs.empty())
		lines.back().pieces.push_back({});
	auto marker = [&](size_t run, size_t begin, size_t end) {
		lines.back().pieces.push_back({run, begin, end, lines.back().width});
	};
	auto newline = [&](size_t run, size_t at) {
		lines.emplace_back();
		marker(run, at, at);
	};
	auto mergeable = [&](size_t run, size_t begin) {
		const auto& pieces = lines.back().pieces;
		if (pieces.empty())
			return false;
		const auto& last = pieces.back();
		return last.run == run && last.end == begin && !controlPiece(runs, last)
		    && (last.width > 0 || last.begin == last.end);
	};
	// Glyph width of [begin, end) without piece chrome; `raw` is the pre-shaped token width when known.
	auto glyphs = [&](size_t run, size_t begin, size_t end, float raw) {
		return raw >= 0
		         ? raw
		         : fonts_.width(fontFor(runs[run].style), std::string_view(runs[run].text).substr(begin, end - begin));
	};
	auto chrome = [&](size_t run, size_t begin, size_t end) {
		return begin == end ? 0.0f : codePadding(runs[run].style, fonts_) * 2 + iconGap(runs, fonts_, run, end);
	};
	// Width of the last piece extended to `end`, summing shaped widths instead of re-shaping the merged text.
	auto merged = [&](size_t run, size_t begin, size_t end, float raw) {
		const auto& last = lines.back().pieces.back();
		return last.width - chrome(run, last.begin, last.end) + glyphs(run, begin, end, raw)
		     + chrome(run, last.begin, end);
	};
	// Re-measures each piece of the line as one string (as it is drawn), placing them again.
	auto settle = [&](Line& line) {
		float x = 0;
		for (auto& piece : line.pieces) {
			piece.x = x;
			if (!piece.ellipsis && piece.begin != piece.end && !controlPiece(runs, piece))
				piece.width = glyphs(piece.run, piece.begin, piece.end, -1) + chrome(piece.run, piece.begin, piece.end);
			x += piece.width;
		}
		line.width = x;
		line.slack = 0;
	};
	auto projected = [&](size_t run, size_t begin, size_t end, float raw = -1) {
		auto& line = lines.back();
		const auto estimate = [&] {
			if (mergeable(run, begin))
				return line.pieces.back().x + merged(run, begin, end, raw);
			return line.width + glyphs(run, begin, end, raw) + chrome(run, begin, end);
		};
		const float approximate = estimate();
		// Close to the edge the rounding error could decide the wrap: measure exactly.
		if (line.slack > 0 && approximate <= width && approximate + line.slack + 1 > width) {
			settle(line);
			return estimate();
		}
		return approximate;
	};
	// `parts`: how many separately measured tokens `raw` sums.
	auto place = [&](size_t run, size_t begin, size_t end, float raw = -1, int parts = 1) {
		auto& line = lines.back();
		if (mergeable(run, begin)) {
			auto& last = line.pieces.back();
			last.width = merged(run, begin, end, raw);
			last.end = end;
			line.width = last.x + last.width;
			line.slack += static_cast<float>(parts);
		} else {
			const float measured = glyphs(run, begin, end, raw) + chrome(run, begin, end);
			line.pieces.push_back({run, begin, end, line.width, measured});
			line.width += measured;
			line.slack += static_cast<float>(parts - 1);
		}
	};
	auto split = [&](size_t tokenIndex) {
		const auto& token = tokens[tokenIndex];
		const auto value = std::string_view(runs[token.run].text);
		for (size_t begin = token.begin; begin < token.end;) {
			const float raw = begin == token.begin ? tokenWidths[tokenIndex] : -1;
			if (projected(token.run, begin, token.end, raw) <= width) {
				place(token.run, begin, token.end, raw);
				break;
			}
			size_t measureBegin = begin;
			float origin = lines.back().width;
			if (mergeable(token.run, begin)) {
				measureBegin = lines.back().pieces.back().begin;
				origin = lines.back().pieces.back().x;
			}
			const float textBudget = std::max(0.0f, width - origin - codePadding(runs[token.run].style, fonts_) * 2);
			const size_t length = fonts_.fit(fontFor(runs[token.run].style),
			                                 value.substr(measureBegin, token.end - measureBegin), textBudget);
			size_t end = boundaryBefore(value, measureBegin + std::min(length, token.end - measureBegin));
			while (end > begin && projected(token.run, begin, end) > width)
				end = boundaryBefore(value, end - 1);
			if (end <= begin) {
				if (lines.back().width > 0) {
					newline(token.run, begin);
					continue;
				}
				end = boundaryAfter(value, begin);
			}
			place(token.run, begin, end);
			begin = end;
			if (begin < token.end)
				newline(token.run, begin);
		}
	};
	const float tabWidth = fonts_.monoAdvance() * 8;
	for (size_t index = 0; index < tokens.size();) {
		const auto& token = tokens[index];
		if (token.kind == TokenKind::Newline || token.kind == TokenKind::Skip) {
			marker(token.run, token.begin, token.end);
			if (token.kind == TokenKind::Newline)
				newline(token.run, token.end);
			else
				marker(token.run, token.end, token.end);
			++index;
			continue;
		}
		if (token.kind == TokenKind::Tab) {
			auto advance = [&] {
				return (std::floor(lines.back().width / tabWidth) + 1) * tabWidth - lines.back().width;
			};
			const float gap =
			    iconGap(runs, fonts_, token.run, token.end) + codePadding(runs[token.run].style, fonts_) * 2;
			if (options.wrap != Wrap::None && lines.back().width > 0 && lines.back().width + advance() + gap > width)
				newline(token.run, token.begin);
			float measured = advance();
			if (options.wrap != Wrap::None)
				measured = std::min(measured, std::max(0.0f, width - lines.back().width - gap));
			auto& line = lines.back();
			line.pieces.push_back({token.run, token.begin, token.end, line.width, measured + gap});
			line.width += measured + gap;
			++index;
			continue;
		}
		if (options.wrap == Wrap::None) {
			const size_t first = index;
			size_t end = token.end;
			float raw = tokenWidths[index];
			++index;
			while (index < tokens.size() && tokens[index].run == token.run
			       && (tokens[index].kind == TokenKind::Text || tokens[index].kind == TokenKind::Space)) {
				raw += tokenWidths[index];
				end = tokens[index++].end;
			}
			place(token.run, token.begin, end, raw, static_cast<int>(index - first));
			continue;
		}
		if (options.wrap == Wrap::Word && token.kind == TokenKind::Text) {
			size_t end = index + 1;
			float measured = projected(token.run, token.begin, token.end, tokenWidths[index]);
			while (end < tokens.size() && tokens[end].kind == TokenKind::Text) {
				const auto& next = tokens[end];
				measured += tokenWidths[end++] + chrome(next.run, next.begin, next.end);
			}
			// A style boundary is not a word boundary.
			if (lines.back().width > 0 && measured > width)
				newline(token.run, token.begin);
			for (; index < end; ++index)
				split(index);
			continue;
		}
		if (options.wrap == Wrap::Word && token.kind == TokenKind::Space && lines.back().width > 0
		    && projected(token.run, token.begin, token.end, tokenWidths[index]) > width) {
			newline(token.run, token.begin);
			marker(token.run, token.begin, token.end);
		} else
			split(index);
		++index;
	}
	// Pieces then match what is drawn: the caret, the next piece and line alignment depend on it.
	for (auto& line : lines)
		if (line.slack > 0)
			settle(line);

	const bool clampLines = options.maxLines > 0 && lines.size() > static_cast<size_t>(options.maxLines);
	std::vector<Line> visible;
	if (!clampLines)
		visible = std::move(lines);
	else {
		result.clipped = true;
		const size_t count = static_cast<size_t>(options.maxLines);
		visible.reserve(count);
		if (options.truncate == Truncate::End) {
			for (size_t index = 0; index < count; ++index)
				visible.push_back(std::move(lines[index]));
			visible.back() = truncateLine(runs, fonts_, visible.back(), visible.back(), width, Truncate::End);
		} else if (options.truncate == Truncate::Start) {
			for (size_t index = lines.size() - count; index < lines.size(); ++index)
				visible.push_back(std::move(lines[index]));
			visible.front() = truncateLine(runs, fonts_, visible.front(), visible.front(), width, Truncate::Start);
		} else {
			const size_t head = (count - 1) / 2, tail = count - head - 1;
			for (size_t index = 0; index < head; ++index)
				visible.push_back(std::move(lines[index]));
			visible.push_back(
			    truncateLine(runs, fonts_, lines[head], lines[lines.size() - tail - 1], width, Truncate::Middle));
			for (size_t index = lines.size() - tail; index < lines.size(); ++index)
				visible.push_back(std::move(lines[index]));
		}
	}
	const float lineHeight = result.lineHeight;
	result.lineWidths.reserve(visible.size());
	size_t pieceCount = 0;
	for (const auto& line : visible)
		pieceCount += line.pieces.size();
	result.pieces.reserve(pieceCount);
	for (size_t index = 0; index < visible.size(); ++index) {
		auto& line = visible[index];
		if (line.width > width) {
			line = truncateLine(runs, fonts_, line, line, width, options.truncate);
			result.clipped = true;
		}
		result.width = std::max(result.width, line.width);
		result.lineWidths.push_back(line.width);
		for (auto piece : line.pieces) {
			piece.line = static_cast<int>(index);
			result.pieces.push_back(piece);
		}
	}
	result.height = static_cast<float>(visible.size()) * lineHeight;
	if (options.caret) {
		size_t target = std::min(*options.caret, bases.back());
		if (!runs.empty()) {
			const auto found = std::upper_bound(bases.begin(), bases.end(), target);
			const size_t run = std::min(static_cast<size_t>(found - bases.begin() - 1), runs.size() - 1);
			target = bases[run] + boundaryBefore(runs[run].text, target - bases[run]);
		}
		for (const auto& piece : result.pieces) {
			if (piece.ellipsis)
				continue;
			const size_t begin = bases[piece.run] + piece.begin, end = bases[piece.run] + piece.end;
			if (target < begin || target > end || (newlinePiece(runs, piece) && target == end))
				continue;
			float at = piece.x;
			if (target == end)
				at += piece.width;
			else if (piece.width > 0 && !controlPiece(runs, piece))
				at += codePadding(runs[piece.run].style, fonts_)
				    + fonts_.width(fontFor(runs[piece.run].style),
				                   std::string_view(runs[piece.run].text).substr(piece.begin, target - begin));
			result.caret = SDL_FPoint{at, static_cast<float>(piece.line) * lineHeight};
			if (target < end)
				break;
		}
		if (runs.empty())
			result.caret = SDL_FPoint{0, 0};
	}
	return result;
}

size_t Engine::CacheHash::operator()(const CacheKey& key) const {
	size_t hash = std::hash<std::string_view>{}(key.value);
	hash ^= static_cast<size_t>(key.color) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	return hash ^ (static_cast<size_t>(key.font) + 0x9e3779b9u + (hash << 6) + (hash >> 2));
}

const Engine::Cached* Engine::cached(FontId font, SDL_Color color, std::string_view value) {
	const CacheKey key{font, colorKey(color), value};
	if (const auto found = index_.find(key); found != index_.end()) {
		cache_.splice(cache_.begin(), cache_, found->second);
		return &*found->second;
	}
	std::unique_ptr<TTF_Text, TextDeleter> text(
	    TTF_CreateText(engine_.get(), &fonts_.get(font), value.data(), value.size()));
	if (!text || !TTF_SetTextColor(text.get(), color.r, color.g, color.b, color.a))
		return nullptr;
	TTF_SubString first{};
	const float top = TTF_GetTextSubString(text.get(), 0, &first) ? static_cast<float>(first.rect.y) : 0;
	cache_.push_front({std::string(value), font, key.color, std::move(text), top});
	index_.emplace(CacheKey{font, key.color, cache_.front().value}, cache_.begin());
	if (cache_.size() > cacheLimit) {
		const auto last = std::prev(cache_.end());
		index_.erase(CacheKey{last->font, last->color, last->value});
		cache_.erase(last);
	}
	return &cache_.front();
}

void Engine::paintSelection(const Runs& runs, const Layout& layout, float x, float y, float lineHeight, float opacity) {
	const auto& selection = *selection_;
	const float infinity = std::numeric_limits<float>::max();
	bool first = true;
	for (const auto& piece : layout.pieces) {
		const float top = y + static_cast<float>(piece.line) * lineHeight, bottom = top + lineHeight;
		// The run of lines between the two points is selected whole; the end lines only from/to their point.
		if (selection.from.y >= bottom || selection.to.y < top)
			continue;
		const float start = selection.from.y < top ? -infinity : selection.from.x;
		const float end = selection.to.y >= bottom ? infinity : selection.to.x;
		const float left = x + piece.x;
		if (piece.ellipsis || runs[piece.run].style.chrome || end <= left || start >= left + piece.width)
			continue;
		const auto value = std::string_view(runs[piece.run].text).substr(piece.begin, piece.end - piece.begin);
		const auto font = fontFor(runs[piece.run].style);
		const float padding = codePadding(runs[piece.run].style, fonts_);
		auto offset = [&](float at) {
			if (at <= left + padding)
				return size_t{0};
			if (at >= left + piece.width - padding)
				return value.size();
			return boundaryBefore(value, fonts_.fit(font, value, at - left - padding));
		};
		size_t from = offset(start), to = offset(end);
		if (selection.word) {
			// Grow from the glyph under the point to its word; a non-word glyph selects itself.
			auto wordByte = [&](size_t at) {
				const auto b = static_cast<unsigned char>(value[at]);
				return b >= 0x80 || std::isalnum(b) || b == '_';
			};
			if (from >= value.size() || controlPiece(runs, piece))
				continue;
			to = boundaryAfter(value, from);
			if (wordByte(from)) {
				while (from > 0 && wordByte(from - 1))
					--from;
				while (to < value.size() && wordByte(to))
					++to;
			}
		}
		const bool control = controlPiece(runs, piece);
		if (from >= to && !control)
			continue;
		const float x0 = from == 0 ? left : left + padding + fonts_.width(font, value.substr(0, from));
		const float x1 =
		    to == value.size() ? left + piece.width : left + padding + fonts_.width(font, value.substr(0, to));
		auto color = withAlpha(palette::accent, 70);
		color.a = static_cast<Uint8>(std::lround(static_cast<float>(color.a) * opacity));
		setColor(renderer_, color);
		const SDL_FRect rect{x0, top, std::max(1.0f, x1 - x0), lineHeight};
		SDL_RenderFillRect(&renderer_, &rect);
		// Each drawn block starts a new output line; inside a block only hard breaks do (soft wraps keep their spaces).
		if (first && !selectedText_.empty() && selectedText_.back() != '\n')
			selectedText_.push_back('\n');
		first = false;
		if (control)
			selectedText_.push_back(newlinePiece(runs, piece) ? '\n' : '\t');
		else if (font != FontId::Icons)
			selectedText_.append(value.substr(from, to - from)); // Icon glyphs are decoration.
	}
}

template <typename Fill>
void Engine::rangeRects(const Runs& runs, const Layout& layout, float x, float y, size_t begin, size_t end,
                        Fill fill) const {
	const float lineHeight = layout.lineHeight > 0 ? layout.lineHeight : fonts_.lineHeight();
	size_t base = 0, run = 0;
	for (const auto& piece : layout.pieces) {
		while (run < piece.run)
			base += runs[run++].text.size();
		const auto& style = runs[piece.run].style;
		const size_t from = std::max(begin, base + piece.begin), to = std::min(end, base + piece.end);
		if (from > to || (from == to && begin != end) || piece.ellipsis || style.chrome
		    || fontFor(style) == FontId::Icons || controlPiece(runs, piece))
			continue;
		const auto value = std::string_view(runs[piece.run].text).substr(piece.begin, piece.end - piece.begin);
		const float padding = codePadding(style, fonts_);
		const auto font = fontFor(style);
		const float left = x + piece.x;
		const float x0 = from == base + piece.begin
		                   ? left
		                   : left + padding + fonts_.width(font, value.substr(0, from - base - piece.begin));
		const float x1 = to == base + piece.end
		                   ? left + piece.width
		                   : left + padding + fonts_.width(font, value.substr(0, to - base - piece.begin));
		fill(SDL_FRect{x0, y + static_cast<float>(piece.line) * lineHeight, std::max(1.0f, x1 - x0), lineHeight});
		if (begin == end)
			break;
	}
}

void Engine::paintHighlight(const Runs& runs, const Layout& layout, float x, float y, size_t begin, size_t end,
                            bool current, float opacity) {
	rangeRects(runs, layout, x, y, begin, end, [&](const SDL_FRect& rect) {
		auto color = withAlpha(palette::warning, current ? 125 : 58);
		color.a = static_cast<Uint8>(std::lround(static_cast<float>(color.a) * opacity));
		gfx::fill(renderer_, rect, color);
		if (current) {
			color = withAlpha(palette::warning, static_cast<Uint8>(std::lround(220 * opacity)));
			gfx::roundedBorder(renderer_, rect, 0, color, std::max(1.0f, fonts_.size() / 15));
		}
	});
}

void Engine::draw(const Runs& runs, const Layout& layout, float x, float y, float opacity, ByteRange selected) {
	if (capture) {
		SDL_Rect rect{};
		SDL_GetRenderClipRect(&renderer_, &rect);
		capture(runs, layout, x, y,
		        {static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.w),
		         static_cast<float>(rect.h)});
		return;
	}
	if (!engine_)
		return;
	const float lineHeight = layout.lineHeight > 0 ? layout.lineHeight : fonts_.lineHeight();
	const float baseline =
	    layout.ascent > 0 ? layout.ascent : static_cast<float>(TTF_GetFontAscent(&fonts_.get(FontId::Ui)));
	const float thickness = std::max(1.0f, std::floor(fonts_.size() / 15));
	const double phase = static_cast<double>(SDL_GetTicks()) * 0.003;
	SDL_Color previousColor;
	SDL_BlendMode previousBlend;
	SDL_GetRenderDrawColor(&renderer_, &previousColor.r, &previousColor.g, &previousColor.b, &previousColor.a);
	SDL_GetRenderDrawBlendMode(&renderer_, &previousBlend);
	SDL_SetRenderDrawBlendMode(&renderer_, SDL_BLENDMODE_BLEND);
	// Paint all backgrounds first so glyph overhangs survive style boundaries.
	for (const auto& piece : layout.pieces) {
		const auto& style = runs[piece.run].style;
		if (style.bg.a == 0 || piece.width == 0)
			continue;
		auto background = style.bg;
		background.a = static_cast<Uint8>(std::lround(static_cast<float>(background.a) * opacity));
		const SDL_FRect rect{x + piece.x, y + static_cast<float>(piece.line) * lineHeight, piece.width, lineHeight};
		if (style.inlineCode && !piece.ellipsis)
			gfx::rounded(renderer_, rect, fonts_.size() * 0.2f, background);
		else {
			setColor(renderer_, background);
			SDL_RenderFillRect(&renderer_, &rect);
		}
	}
	if (decorate)
		decorate(runs, layout, x, y, opacity);
	if (selection_)
		paintSelection(runs, layout, x, y, lineHeight, opacity);
	if (selected.begin < selected.end) {
		auto color = withAlpha(palette::accent, 65);
		color.a = static_cast<Uint8>(std::lround(static_cast<float>(color.a) * opacity));
		rangeRects(runs, layout, x, y, selected.begin, selected.end,
		           [&](const SDL_FRect& rect) { gfx::fill(renderer_, rect, color); });
	}
	for (const auto& piece : layout.pieces) {
		const auto& style = runs[piece.run].style;
		const float left = x + piece.x, top = y + static_cast<float>(piece.line) * lineHeight;
		const float padding = piece.ellipsis ? 0 : codePadding(style, fonts_);
		std::optional<std::reference_wrapper<TTF_Text>> text;
		float textY = top;
		if (!controlPiece(runs, piece)) {
			const auto value = valueFor(runs, piece);
			if (!value.empty()) {
				const FontId font = fontFor(style, piece);
				if (const auto entry = cached(font, style.color, value)) {
					text = *entry->text;
					textY += baseline - static_cast<float>(TTF_GetFontAscent(&fonts_.get(font))) - entry->top;
				}
			}
		}
		auto colorAt = [&](float at) {
			auto color = style.color;
			if (style.fx != 0) {
				const double wavePhase = phase - (style.fx == 1 ? static_cast<double>(at / (fonts_.size() * 4)) : 0);
				color.a = static_cast<Uint8>(
				    std::lround(static_cast<double>(color.a) * (0.775 + 0.225 * std::sin(wavePhase))));
			}
			color.a = static_cast<Uint8>(std::lround(static_cast<float>(color.a) * opacity));
			return color;
		};
		auto paint = [&](SDL_Color color) {
			if (text) {
				if (style.fx != 0 || opacity != 1)
					TTF_SetTextColor(&text->get(), color.r, color.g, color.b, color.a);
				// Whole pixels: glyphs come from a nearest-sampled atlas packed without gaps, and at a
				// fractional position some drivers (seen on Windows 10 at 100 %) sample the column of
				// the neighbouring glyph, leaving stray ticks and cut edges.
				TTF_DrawRendererText(&text->get(), std::round(left + padding), std::round(textY));
			}
			if (piece.width == 0 || newlinePiece(runs, piece))
				return;
			setColor(renderer_, color);
			const float underlineY = top + std::min(lineHeight - thickness, baseline + thickness);
			if (style.underline)
				rule(renderer_, left, underlineY, piece.width, thickness);
			if (style.strike)
				rule(renderer_, left, top + baseline - fonts_.size() * 0.3f, piece.width, thickness);
			if (style.wavy)
				wave(renderer_, left, underlineY, piece.width, fonts_.size());
		};
		if (style.fx == 1 && piece.width > 0) {
			const bool clipped = SDL_RenderClipEnabled(&renderer_);
			SDL_Rect previousClip{};
			if (clipped)
				SDL_GetRenderClipRect(&renderer_, &previousClip);
			const float bandWidth = std::max(8.0f, fonts_.size() * 0.75f);
			// Reuse the shaped text, tinting clipped bands instead of splitting glyphs.
			for (float at = 0; at < piece.width; at += bandWidth) {
				SDL_Rect clip{static_cast<int>(std::floor(left + at)), static_cast<int>(std::floor(top)), 0,
				              static_cast<int>(std::ceil(top + lineHeight)) - static_cast<int>(std::floor(top))};
				const float end = std::min(piece.width, at + bandWidth);
				const int bandEnd =
				    static_cast<int>(end == piece.width ? std::ceil(left + end) : std::floor(left + end));
				clip.w = bandEnd - clip.x;
				if (clipped) {
					const int right = std::min(clip.x + clip.w, previousClip.x + previousClip.w);
					const int bottom = std::min(clip.y + clip.h, previousClip.y + previousClip.h);
					clip.x = std::max(clip.x, previousClip.x);
					clip.y = std::max(clip.y, previousClip.y);
					clip.w = right - clip.x;
					clip.h = bottom - clip.y;
				}
				if (clip.w <= 0 || clip.h <= 0)
					continue;
				SDL_SetRenderClipRect(&renderer_, &clip);
				paint(colorAt(piece.x + at + bandWidth * 0.5f));
			}
			SDL_SetRenderClipRect(&renderer_, clipped ? &previousClip : nullptr);
		} else
			paint(colorAt(piece.x));
		// Animation never changes the cache key or another run's base colour.
		if (text && (style.fx != 0 || opacity != 1))
			TTF_SetTextColor(&text->get(), style.color.r, style.color.g, style.color.b, style.color.a);
	}
	setColor(renderer_, previousColor);
	SDL_SetRenderDrawBlendMode(&renderer_, previousBlend);
}

Hit Engine::hit(const Runs& runs, const Layout& layout, float x, float y) {
	if (layout.pieces.empty())
		return {};
	const float lineHeight = layout.lineHeight > 0 ? layout.lineHeight : fonts_.lineHeight();
	const float baseline =
	    layout.ascent > 0 ? layout.ascent : static_cast<float>(TTF_GetFontAscent(&fonts_.get(FontId::Ui)));
	const int line = static_cast<int>(
	    std::clamp(std::floor(y / lineHeight), 0.0f, static_cast<float>(layout.lineWidths.size() - 1)));
	size_t selected = 0;
	float distance = std::numeric_limits<float>::max();
	for (size_t index = 0; index < layout.pieces.size(); ++index) {
		const auto& piece = layout.pieces[index];
		if (piece.line != line)
			continue;
		const float next = std::max({piece.x - x, x - piece.x - piece.width, 0.0f});
		if (next <= distance) {
			distance = next;
			selected = index;
		}
		if (piece.width > 0 && x >= piece.x && x < piece.x + piece.width)
			break;
	}
	const auto& piece = layout.pieces[selected];
	if (piece.ellipsis) {
		size_t before = globalByte(runs, piece.run, piece.begin), after = before;
		for (size_t index = selected; index > 0;) {
			const auto& previous = layout.pieces[--index];
			if (previous.line != line)
				break;
			if (!previous.ellipsis) {
				before = globalByte(runs, previous.run, previous.end);
				break;
			}
		}
		for (size_t index = selected + 1; index < layout.pieces.size(); ++index) {
			const auto& next = layout.pieces[index];
			if (next.line != line)
				break;
			if (!next.ellipsis) {
				after = globalByte(runs, next.run, next.begin);
				break;
			}
		}
		return {x < piece.x + piece.width * 0.5f ? before : after, {}};
	}
	const auto& style = runs[piece.run].style;
	const bool inside = y >= 0 && y < layout.height && x >= piece.x && x < piece.x + piece.width;
	Hit result{globalByte(runs, piece.run, piece.begin), inside ? std::string_view(style.href) : std::string_view{}};
	if (controlPiece(runs, piece)) {
		if (x >= piece.x + piece.width * 0.5f && !newlinePiece(runs, piece))
			result.byte += piece.end - piece.begin;
		return result;
	}
	const auto value = valueFor(runs, piece);
	if (value.empty())
		return result;
	const float padding = codePadding(style, fonts_);
	if (padding > 0 && x >= piece.x + piece.width - padding) {
		result.byte += piece.end - piece.begin;
		return result;
	}
	const FontId font = fontFor(style);
	if (const auto text = cached(font, style.color, value)) {
		TTF_SubString substring{};
		const float offset = baseline - static_cast<float>(TTF_GetFontAscent(&fonts_.get(font))) - text->top;
		const float localX = x - piece.x - padding;
		const float localY = y - static_cast<float>(line) * lineHeight - offset;
		if (TTF_GetTextSubStringForPoint(text->text.get(), static_cast<int>(std::floor(localX)),
		                                 static_cast<int>(std::floor(localY)), &substring)) {
			const bool rtl = (substring.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL;
			const bool right =
			    localX >= static_cast<float>(substring.rect.x) + static_cast<float>(substring.rect.w) * 0.5f;
			const int offsetByte = substring.offset + ((right != rtl) ? substring.length : 0);
			result.byte += boundaryBefore(value, static_cast<size_t>(std::max(0, offsetByte)));
		}
	}
	return result;
}

void Engine::clear() {
	index_.clear();
	cache_.clear();
	pictures_.clear();
	pictureBytes_ = 0;
}

void Engine::drawImage(const gfx::Image& image, const SDL_FRect& rect, float opacity) {
	if (capture)
		return; // Off-screen text gathering must not decode or upload transcript pictures.
	// GPU drivers keep freed texture memory pooled, so the budget, not trimming, bounds what
	// pictures cost: about two full-width screenshots; this frame's pictures are never evicted.
	constexpr size_t pictureBudget = 8 * 1024 * 1024;
	const int width = std::max(1, static_cast<int>(std::lround(rect.w))),
	          height = std::max(1, static_cast<int>(std::lround(rect.h)));
	// Raster pictures are never uploaded larger than they are drawn (or than their own size);
	// a texture within 1.5× of that is kept so resizing doesn't re-decode every frame. Vector
	// pictures render at exactly the drawn size.
	const int naturalWidth = static_cast<int>(image.width()), naturalHeight = static_cast<int>(image.height());
	const int wantWidth = image.vector() ? width : std::min(width, naturalWidth),
	          wantHeight = image.vector() ? height : std::min(height, naturalHeight);
	const auto fits = [&](const Picture& picture) {
		if (image.vector())
			return picture.width == width && picture.height == height;
		return picture.width >= wantWidth && picture.height >= wantHeight && picture.width * 2 <= wantWidth * 3 + 2;
	};
	auto found = std::find_if(pictures_.begin(), pictures_.end(),
	                          [&](const Picture& picture) { return picture.id == image.id(); });
	if (found != pictures_.end() && !fits(*found)) {
		pictureBytes_ -= found->bytes();
		pictures_.erase(found);
		found = pictures_.end();
	}
	if (found == pictures_.end()) {
		const auto pixels = image.pixels(wantWidth, wantHeight);
		if (pixels.argb.empty())
			return;
		std::unique_ptr<SDL_Texture, TextureDeleter> texture(SDL_CreateTexture(
		    &renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, pixels.width, pixels.height));
		if (!texture)
			return;
		SDL_UpdateTexture(texture.get(), nullptr, pixels.argb.data(), pixels.width * 4);
		SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
		pictures_.push_front({image.id(), pixels.width, pixels.height, std::move(texture)});
		pictureBytes_ += pictures_.front().bytes();
		// Least recently drawn first; never one drawn in this frame or the last. Counting frames, not
		// milliseconds: a frame slowed by this very decode used to make the pictures drawn earlier in
		// it look stale, so two pictures over the budget (wallpaper and a screenshot) evicted and
		// re-decoded each other every frame.
		while (pictureBytes_ > pictureBudget && pictures_.size() > 1 && pictures_.back().drawnFrame + 1 < frame_) {
			pictureBytes_ -= pictures_.back().bytes();
			pictures_.pop_back();
		}
		found = pictures_.begin();
	} else if (found != pictures_.begin())
		pictures_.splice(pictures_.begin(), pictures_, found);
	found->drawn = SDL_GetTicks();
	found->drawnFrame = frame_;
	// Premultiplied pixels fade by scaling colour and alpha together.
	SDL_SetTextureColorModFloat(found->texture.get(), opacity, opacity, opacity);
	SDL_SetTextureAlphaModFloat(found->texture.get(), opacity);
	SDL_RenderTexture(&renderer_, found->texture.get(), nullptr, &rect);
}

void Engine::trimPictures() {
	// A few seconds' grace, so scrolling back and forth doesn't decode the same picture repeatedly.
	constexpr Uint64 keep = 5000;
	const Uint64 now = SDL_GetTicks();
	for (auto it = pictures_.begin(); it != pictures_.end();)
		if (now - it->drawn > keep) {
			pictureBytes_ -= it->bytes();
			it = pictures_.erase(it);
		} else
			++it;
}

} // namespace text
