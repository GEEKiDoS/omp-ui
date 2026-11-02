#include "md.h"

#include "code.h"
#include "gfx/draw.h"
#include "view/hit_targets.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace kinds {
namespace {

std::string_view trimLeft(std::string_view s) {
	const auto at = s.find_first_not_of(" \t");
	return at == std::string_view::npos ? std::string_view{} : s.substr(at);
}
std::string_view trim(std::string_view s) {
	s = trimLeft(s);
	const auto at = s.find_last_not_of(" \t\r");
	return at == std::string_view::npos ? std::string_view{} : s.substr(0, at + 1);
}
int indentOf(std::string_view s) {
	int result = 0;
	for (const char c : s) {
		if (c == ' ')
			++result;
		else if (c == '\t')
			result += 4;
		else
			break;
	}
	return result;
}
bool fence(std::string_view line) {
	line = trimLeft(line);
	return line.starts_with("```") || line.starts_with("~~~");
}
bool rule(std::string_view line) {
	line = trim(line);
	if (line.size() < 3 || (line.front() != '-' && line.front() != '*' && line.front() != '_'))
		return false;
	int count = 0;
	for (const char c : line) {
		if (c == line.front())
			++count;
		else if (c != ' ')
			return false;
	}
	return count >= 3;
}
size_t listMarker(std::string_view line, std::string& marker) {
	if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
		marker = "•";
		return 2;
	}
	size_t at = 0;
	while (at < line.size() && at < 9 && std::isdigit(static_cast<unsigned char>(line[at])))
		++at;
	if (at && at + 1 < line.size() && (line[at] == '.' || line[at] == ')') && line[at + 1] == ' ') {
		marker = line.substr(0, at + 1);
		return at + 2;
	}
	return 0;
}
std::vector<std::string> tableCells(std::string_view line) {
	line = trim(line);
	if (line.starts_with('|'))
		line.remove_prefix(1);
	if (line.ends_with('|'))
		line.remove_suffix(1);
	std::vector<std::string> cells;
	std::string value;
	for (size_t at = 0; at < line.size(); ++at) {
		if (line[at] == '\\' && at + 1 < line.size() && line[at + 1] == '|') {
			value += '|';
			++at;
		} else if (line[at] == '|') {
			cells.emplace_back(trim(value));
			value.clear();
		} else
			value += line[at];
	}
	cells.emplace_back(trim(value));
	return cells;
}
bool tableSeparator(std::string_view line) {
	return line.find('-') != std::string_view::npos && line.find('|') != std::string_view::npos
	    && line.find_first_not_of("|-: \t\r") == std::string_view::npos;
}
size_t headingLevel(std::string_view line) {
	const auto end = line.find_first_not_of('#');
	const auto level = end == std::string_view::npos ? line.size() : end;
	return level > 0 && level <= 6 && (level == line.size() || line[level] == ' ') ? level : 0;
}
void parseBlocks(std::string_view source, size_t offset, std::vector<MdBlock>& blocks) {
	struct Line {
		std::string_view text;
		size_t from;
	};
	std::vector<Line> lines;
	for (size_t at = 0; at <= source.size();) {
		const auto end = source.find('\n', at);
		const auto stop = end == std::string_view::npos ? source.size() : end;
		auto value = source.substr(at, stop - at);
		if (value.ends_with('\r'))
			value.remove_suffix(1);
		lines.push_back({value, offset + at});
		if (end == std::string_view::npos)
			break;
		at = end + 1;
	}
	bool blank = true;
	for (size_t at = 0; at < lines.size(); ++at) {
		const auto line = lines[at].text, value = trim(line);
		MdBlock block;
		block.from = lines[at].from;
		if (fence(line)) {
			block.kind = MdBlock::Kind::Code;
			const auto delimiter = trimLeft(line).substr(0, 3);
			block.lang = trim(trimLeft(line).substr(3));
			for (++at; at < lines.size(); ++at) {
				if (trimLeft(lines[at].text).starts_with(delimiter) && trim(lines[at].text).size() <= 4) {
					block.closed = true;
					break;
				}
				if (at > 0 && lines[at - 1].from != block.from)
					block.text += '\n';
				block.text += lines[at].text;
			}
		} else if (value.empty()) {
			blank = true;
			continue;
		} else if (const auto level = headingLevel(value)) {
			block.kind = MdBlock::Kind::Heading;
			block.level = static_cast<int>(level);
			block.text = trim(value.substr(level));
		} else if (rule(line))
			block.kind = MdBlock::Kind::Rule;
		else if (value.front() == '>') {
			const auto body = trimLeft(value.substr(1));
			if (!blank && !blocks.empty() && blocks.back().kind == MdBlock::Kind::Quote) {
				blocks.back().text += body.empty() ? "\n" : " " + std::string(body);
				continue;
			}
			block.kind = MdBlock::Kind::Quote;
			block.text = body;
		} else if (value.find('|') != std::string_view::npos && at + 1 < lines.size()
		           && tableSeparator(lines[at + 1].text)) {
			block.kind = MdBlock::Kind::Table;
			block.rows.push_back(tableCells(line));
			for (const auto& cell : tableCells(lines[at + 1].text))
				block.align.push_back(cell.starts_with(':') ? (cell.ends_with(':') ? 1 : 0)
				                      : cell.ends_with(':') ? 2
				                                            : 0);
			for (at += 2; at < lines.size() && lines[at].text.find('|') != std::string_view::npos
			              && !trim(lines[at].text).empty();
			     ++at)
				block.rows.push_back(tableCells(lines[at].text));
			--at;
		} else if (const auto prefix = listMarker(value, block.marker)) {
			block.kind = MdBlock::Kind::Item;
			block.level = indentOf(line) / 2;
			auto body = value.substr(prefix);
			if (body.starts_with("[ ]") || body.starts_with("[x]") || body.starts_with("[X]")) {
				block.marker = body[1] == ' ' ? "☐" : "☑";
				body = trimLeft(body.substr(3));
			}
			block.text = body;
		} else if (!blank && !blocks.empty() && blocks.back().kind == MdBlock::Kind::Item && indentOf(line) >= 2) {
			blocks.back().text += " " + std::string(value);
			continue;
		} else if (!blank && !blocks.empty() && blocks.back().kind == MdBlock::Kind::Paragraph) {
			auto& previous = blocks.back().text;
			if (previous.ends_with("  ")) {
				previous.resize(previous.size() - 2);
				previous += '\n';
			} else if (previous.ends_with('\\'))
				previous.back() = '\n';
			else
				previous += ' ';
			previous += trimLeft(line);
			continue;
		} else
			block.text = trimLeft(line);
		blocks.push_back(std::move(block));
		blank = false;
	}
}
void append(text::Runs& runs, text::Runs source) {
	runs.insert(runs.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}
void applyMarks(text::Runs& runs, const json& marks) {
	if (!marks.is_array() || marks.empty())
		return;
	text::Runs result;
	for (auto& run : runs) {
		size_t at = 0;
		while (at < run.text.size()) {
			size_t match = std::string::npos;
			std::optional<size_t> markIndex;
			for (size_t i = 0; i < marks.size(); ++i) {
				const auto& literal = marks[i].at("t").get_ref<const std::string&>();
				if (literal.empty())
					continue;
				const auto found = run.text.find(literal, at);
				if (found < match) {
					match = found;
					markIndex = i;
				}
			}
			if (!markIndex) {
				result.push_back({run.text.substr(at), run.style});
				break;
			}
			if (match > at)
				result.push_back({run.text.substr(at, match - at), run.style});
			const auto& mark = marks[*markIndex];
			append(result, text::toRuns(json::array({mark}), run.style));
			at = match + mark.at("t").get_ref<const std::string&>().size();
		}
	}
	runs = std::move(result);
}
void compile(MdBlock& block, const text::RunStyle& base, const json& marks) {
	auto style = base;
	if (block.kind == MdBlock::Kind::Heading) {
		style.font = text::FontId::Bold;
		if (block.level <= 2)
			style.color = text::palette::accent;
	}
	if (block.kind == MdBlock::Kind::Quote)
		style.color = text::palette::muted;
	if (block.kind == MdBlock::Kind::Code && block.closed && block.lang == "svg") {
		if (auto picture = gfx::Image::svg(block.text)) {
			block.kind = MdBlock::Kind::Picture;
			block.picture = std::make_shared<const gfx::Image>(std::move(*picture));
			return;
		}
	}
	if (block.kind == MdBlock::Kind::Code) {
		for (size_t at = 0; at <= block.text.size();) {
			const auto end = block.text.find('\n', at), stop = end == std::string::npos ? block.text.size() : end;
			append(block.runs, highlightLine(std::string_view(block.text).substr(at, stop - at), block.lang));
			if (end == std::string::npos)
				break;
			block.runs.push_back({"\n", {.font = text::FontId::Mono}});
			at = end + 1;
		}
	} else if (block.kind == MdBlock::Kind::Table) {
		for (size_t r = 0; r < block.rows.size(); ++r)
			for (size_t c = 0; c < block.rows.front().size(); ++c) {
				auto cellStyle = style;
				if (r == 0)
					cellStyle.font = text::FontId::Bold;
				MdCell cell;
				cell.runs = inlineMarkdown(c < block.rows[r].size() ? block.rows[r][c] : "", cellStyle);
				applyMarks(cell.runs, marks);
				cell.align = c < block.align.size() ? block.align[c] : 0;
				block.cells.push_back(std::move(cell));
			}
	} else {
		block.runs = inlineMarkdown(block.text, style);
		applyMarks(block.runs, marks);
	}
	if (!block.marker.empty())
		block.markerRuns.push_back({block.marker, {.color = text::palette::muted}});
}
bool drawRuns(const text::Runs& runs, const text::Layout& layout, text::Engine& engine, float x, float y,
              view::HitTargets& hits) {
	engine.draw(runs, layout, x, y);
	bool animating = false;
	for (const auto& piece : layout.pieces) {
		const auto& style = runs[piece.run].style;
		const SDL_FRect rect{x + piece.x, y + static_cast<float>(piece.line) * layout.lineHeight, piece.width,
		                     layout.lineHeight};
		if (!style.href.empty())
			hits.add(rect, view::TargetKind::Link, style.href);
		animating |= style.fx != 0 && piece.width > 0;
	}
	return animating;
}

} // namespace

text::Runs inlineMarkdown(std::string_view source, const text::RunStyle& base) {
	text::Runs runs;
	std::string pending;
	bool strong = false, emphasis = false, strike = false;
	auto styled = [&] {
		auto style = base;
		style.font = static_cast<text::FontId>(static_cast<int>(base.font) | (strong ? 1 : 0) | (emphasis ? 2 : 0));
		if (strike) {
			style.strike = true;
			style.color = text::palette::muted;
		}
		return style;
	};
	auto flush = [&] {
		if (!pending.empty()) {
			runs.push_back({std::move(pending), styled()});
			pending.clear();
		}
	};
	for (size_t at = 0; at < source.size(); ++at) {
		const char c = source[at];
		if (c == '\\' && at + 1 < source.size() && std::ispunct(static_cast<unsigned char>(source[at + 1]))) {
			pending += source[++at];
			continue;
		}
		if (c == '`') {
			size_t count = 1;
			while (at + count < source.size() && source[at + count] == '`')
				++count;
			const auto end = source.find(std::string(count, '`'), at + count);
			if (end != std::string_view::npos) {
				flush();
				auto style = styled();
				style.font = text::FontId::Mono;
				style.inlineCode = true;
				style.bg = text::palette::surface2;
				style.color = text::palette::warning;
				auto body = source.substr(at + count, end - at - count);
				if (body.size() > 1 && body.front() == ' ' && body.back() == ' ')
					body = body.substr(1, body.size() - 2);
				runs.push_back({std::string(body), std::move(style)});
				at = end + count - 1;
				continue;
			}
		}
		const bool wordBefore = at > 0 && std::isalnum(static_cast<unsigned char>(source[at - 1]));
		if ((c == '*' || c == '_') && at + 1 < source.size() && source[at + 1] == c
		    && (c != '_' || !wordBefore
		        || (strong
		            && (at + 2 == source.size() || !std::isalnum(static_cast<unsigned char>(source[at + 2])))))) {
			flush();
			strong = !strong;
			++at;
			continue;
		}
		if ((c == '*' || (c == '_' && !wordBefore)
		     || (c == '_' && emphasis
		         && (at + 1 == source.size() || !std::isalnum(static_cast<unsigned char>(source[at + 1])))))
		    && (emphasis || (at + 1 < source.size() && source[at + 1] != ' '))) {
			flush();
			emphasis = !emphasis;
			continue;
		}
		if (c == '~' && at + 1 < source.size() && source[at + 1] == '~') {
			flush();
			strike = !strike;
			++at;
			continue;
		}
		if (c == '[') {
			const auto close = source.find("](", at + 1),
			           end = close == std::string_view::npos ? close : source.find(')', close + 2);
			if (end != std::string_view::npos
			    && source.substr(at + 1, close - at - 1).find('\n') == std::string_view::npos) {
				flush();
				auto style = styled();
				style.href = source.substr(close + 2, end - close - 2);
				style.color = text::palette::accent;
				style.underline = true;
				append(runs, inlineMarkdown(source.substr(at + 1, close - at - 1), style));
				at = end;
				continue;
			}
		}
		const auto url = c == '<' ? at + 1 : at;
		if ((source.substr(url).starts_with("http://") || source.substr(url).starts_with("https://")) && !wordBefore) {
			size_t end = url;
			while (end < source.size() && !std::isspace(static_cast<unsigned char>(source[end])) && source[end] != ')'
			       && source[end] != '>')
				++end;
			while (end > url && (source[end - 1] == '.' || source[end - 1] == ','))
				--end;
			flush();
			auto style = styled();
			style.href = source.substr(url, end - url);
			style.color = text::palette::accent;
			style.underline = true;
			runs.push_back({style.href, style});
			at = end - 1 + (c == '<' && end < source.size() && source[end] == '>' ? 1 : 0);
			continue;
		}
		pending += c;
	}
	flush();
	return runs;
}

void syncMd(const tsp::Node& node, MdCache& cache, const text::Fonts& fonts) {
	const auto source = node.str("text");
	const bool sameStyle = cache.marks == node.prop("marks") && cache.tone == node.str("tone");
	if (cache.fontSize != fonts.size()) {
		for (auto& block : cache.blocks)
			block.width = -1;
		cache.width = -1;
	}
	if (source == cache.source && sameStyle) {
		cache.fontSize = fonts.size();
		return;
	}
	const bool appendOnly = node.flag("stream") && source.size() > cache.source.size()
	                     && source.starts_with(cache.source) && cache.marks == node.prop("marks")
	                     && cache.tone == node.str("tone");
	size_t first = 0, from = 0;
	if (appendOnly && !cache.blocks.empty()) {
		first = cache.blocks.size() - 1;
		from = cache.blocks.back().from;
	}
	cache.blocks.resize(first);
	parseBlocks(source.substr(from), from, cache.blocks);
	text::RunStyle base;
	text::applyTokens(node.str("tone"), base);
	for (size_t i = first; i < cache.blocks.size(); ++i)
		compile(cache.blocks[i], base, node.prop("marks"));
	cache.reparseFrom = from;
	cache.reparseBytes = source.size() - from;
	if (appendOnly)
		SDL_Log("md append %s: reparsed last block from byte %zu (%zu/%zu bytes), kept %zu blocks", node.id.c_str(),
		        from, cache.reparseBytes, source.size(), first);
	if (appendOnly)
		cache.source.append(source.substr(cache.source.size()));
	else
		cache.source = source;
	cache.marks = node.prop("marks");
	cache.tone = node.str("tone");
	cache.fontSize = fonts.size();
	cache.width = -1;
}

SDL_FPoint measureMd(MdCache& cache, text::Engine& engine, const text::Fonts& fonts, float width) {
	width = std::max(0.0f, width);
	if (cache.width == width)
		return {cache.measuredWidth, cache.height};
	cache.width = width;
	const float gap = fonts.size() * gfx::space::md, itemGap = fonts.size() * gfx::space::sm;
	const float pad = fonts.size() * gfx::space::lg, cellPadY = fonts.size() * gfx::space::sm;
	float y = 0;
	for (size_t i = 0; i < cache.blocks.size(); ++i) {
		auto& block = cache.blocks[i];
		if (i) {
			const auto kind = block.kind;
			y += kind == MdBlock::Kind::Item && cache.blocks[i - 1].kind == kind ? itemGap : gap;
			if (kind == MdBlock::Kind::Heading)
				y += gap * 0.5f; // Headings open a new section.
		}
		block.y = y;
		if (block.width == width) {
			y += block.height;
			continue;
		}
		block.width = width;
		block.indent = block.kind == MdBlock::Kind::Quote || block.kind == MdBlock::Kind::Code ? pad : 0;
		if (block.kind == MdBlock::Kind::Item) {
			block.markerLayout =
			    engine.layout(block.markerRuns, {.width = width, .wrap = text::Wrap::None, .maxLines = 1});
			block.indent = static_cast<float>(block.level) * fonts.size() * gfx::space::xl
			             + std::max(fonts.size(), block.markerLayout.width + fonts.size() * gfx::space::sm);
		}
		if (block.kind == MdBlock::Kind::Rule)
			block.height = gap * 2;
		else if (block.kind == MdBlock::Kind::Table) {
			const auto columns = block.rows.front().size();
			float cellWidth = width / static_cast<float>(columns);
			if (width >= 1000000) {
				cellWidth = pad * 2;
				for (const auto& cell : block.cells) {
					const float natural =
					    pad * 2 + engine.layout(cell.runs, {.width = 1000000, .wrap = text::Wrap::None}).width;
					cellWidth = std::max(cellWidth, natural);
				}
			}
			float rowY = 0;
			for (size_t r = 0; r < block.rows.size(); ++r) {
				float height = fonts.lineHeight() + cellPadY * 2;
				for (size_t c = 0; c < columns; ++c) {
					auto& cell = block.cells[r * columns + c];
					cell.width = cellWidth;
					cell.x = static_cast<float>(c) * cellWidth;
					cell.y = rowY;
					cell.layout = engine.layout(cell.runs, {.width = std::max(0.0f, cellWidth - pad * 2)});
					height = std::max(height, cell.layout.height + cellPadY * 2);
				}
				for (size_t c = 0; c < columns; ++c)
					block.cells[r * columns + c].height = height;
				rowY += height;
			}
			block.height = rowY;
		} else if (block.kind == MdBlock::Kind::Picture) {
			const auto size = block.picture->fitted(fonts.size() / 15, width, fonts.lineHeight() * gfx::pictureLines);
			block.layout = {};
			block.layout.width = size.x;
			block.layout.height = block.height = size.y;
		} else {
			block.layout = engine.layout(
			    block.runs,
			    {.width = std::max(0.0f, width - block.indent - (block.kind == MdBlock::Kind::Code ? pad : 0)),
			     .wrap = block.kind == MdBlock::Kind::Code ? text::Wrap::Char : text::Wrap::Word});
			block.height = block.layout.height + (block.kind == MdBlock::Kind::Code ? pad * 2 : 0);
		}
		y += block.height;
	}
	cache.height = y;
	cache.measuredWidth = width;
	if (width >= 1000000) {
		cache.measuredWidth = 0;
		for (const auto& block : cache.blocks) {
			float natural = block.layout.width + block.indent + (block.kind == MdBlock::Kind::Code ? pad : 0);
			if (block.kind == MdBlock::Kind::Table)
				for (const auto& cell : block.cells)
					natural = std::max(natural, cell.x + cell.width);
			if (block.kind == MdBlock::Kind::Rule)
				natural = fonts.size() * 6;
			cache.measuredWidth = std::max(cache.measuredWidth, natural);
		}
	}
	return {cache.measuredWidth, y};
}

bool paintMd(MdCache& cache, text::Engine& engine, const text::Fonts& fonts, SDL_Renderer& renderer,
             const SDL_FRect& rect, view::HitTargets& hits) {
	measureMd(cache, engine, fonts, rect.w);
	const float pad = fonts.size() * gfx::space::lg, cellPadY = fonts.size() * gfx::space::sm, unit = fonts.size() / 15;
	SDL_Rect clip;
	SDL_GetRenderClipRect(&renderer, &clip);
	const SDL_FRect visible{static_cast<float>(clip.x), static_cast<float>(clip.y), static_cast<float>(clip.w),
	                        static_cast<float>(clip.h)};
	bool animating = false;
	for (auto& block : cache.blocks) {
		const SDL_FRect box{rect.x, rect.y + block.y, rect.w, block.height};
		if (!gfx::intersects(box, visible))
			continue;
		if (block.kind == MdBlock::Kind::Rule)
			gfx::line(renderer, {box.x, box.y + box.h * 0.5f}, {box.x + box.w, box.y + box.h * 0.5f},
			          text::palette::border, unit);
		else if (block.kind == MdBlock::Kind::Picture)
			engine.drawImage(*block.picture, {box.x, box.y, block.layout.width, block.layout.height});
		else if (block.kind == MdBlock::Kind::Table) {
			for (const auto& cell : block.cells) {
				const SDL_FRect cellBox{box.x + cell.x, box.y + cell.y, cell.width, cell.height};
				if (cell.y == 0)
					gfx::fill(renderer, cellBox, text::palette::surface2);
				gfx::line(renderer, {cellBox.x, cellBox.y + cellBox.h}, {cellBox.x + cellBox.w, cellBox.y + cellBox.h},
				          text::palette::border, unit);
				const float spare = std::max(0.0f, cell.width - pad * 2 - cell.layout.width);
				animating |= drawRuns(cell.runs, cell.layout, engine,
				                      cellBox.x + pad
				                          + (cell.align == 1   ? spare * 0.5f
				                             : cell.align == 2 ? spare
				                                               : 0),
				                      cellBox.y + cellPadY, hits);
			}
		} else {
			if (block.kind == MdBlock::Kind::Code)
				gfx::rounded(renderer, box, unit * 5, text::palette::surface2);
			if (block.kind == MdBlock::Kind::Quote)
				gfx::fill(renderer, {box.x, box.y, unit * 3, box.h}, text::palette::border);
			if (block.kind == MdBlock::Kind::Item)
				drawRuns(block.markerRuns, block.markerLayout, engine,
				         box.x + static_cast<float>(block.level) * fonts.size() * gfx::space::xl,
				         box.y + block.layout.ascent - block.markerLayout.ascent, hits);
			animating |= drawRuns(block.runs, block.layout, engine, box.x + block.indent,
			                      box.y + (block.kind == MdBlock::Kind::Code ? pad : 0), hits);
			if (block.kind == MdBlock::Kind::Heading && block.level == 1)
				gfx::line(renderer, {box.x, box.y + box.h}, {box.x + box.w, box.y + box.h}, text::palette::border,
				          unit);
		}
	}
	return animating;
}

} // namespace kinds
