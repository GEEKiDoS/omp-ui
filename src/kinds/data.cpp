#include "data.h"

#include "gfx/draw.h"
#include "gfx/icons.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace kinds {
namespace {

constexpr float naturalLimit = 1000000;

text::RunStyle style(SDL_Color color = text::palette::fg, text::FontId font = text::FontId::Ui) {
	text::RunStyle result;
	result.color = color;
	result.font = font;
	return result;
}

SDL_Color tone(std::string_view token, SDL_Color fallback) {
	auto result = style(fallback);
	text::applyTokens(token, result);
	return result.color;
}

const json& prop(const json& value, std::string_view key) {
	static const json absent;
	const auto found = value.find(key);
	return found == value.end() ? absent : *found;
}

std::string_view str(const json& value, std::string_view key, std::string_view fallback = {}) {
	const auto& text = prop(value, key);
	return text.is_string() ? std::string_view(text.get_ref<const std::string&>()) : fallback;
}

void append(text::Runs& target, text::Runs source) {
	target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

size_t addText(DataCache& cache, text::Engine& engine, text::Runs runs) {
	DataCache::Text value;
	value.runs = std::move(runs);
	value.options = {.width = naturalLimit, .wrap = text::Wrap::None};
	value.layout = engine.layout(value.runs, value.options);
	value.naturalWidth = value.layout.width;
	value.animated =
	    std::any_of(value.runs.begin(), value.runs.end(), [](const auto& run) { return run.style.fx != 0; });
	cache.texts.push_back(std::move(value));
	return cache.texts.size() - 1;
}

void layoutText(DataCache::Text& value, text::Engine& engine, float width, text::Wrap wrap,
                text::Truncate truncate = text::Truncate::End, int maxLines = 1) {
	width = std::max(0.0f, width);
	if (value.options.width == width && value.options.wrap == wrap && value.options.truncate == truncate
	    && value.options.maxLines == maxLines)
		return;
	value.options = {.width = width, .wrap = wrap, .truncate = truncate, .maxLines = maxLines};
	value.layout = engine.layout(value.runs, value.options);
}

void placeText(DataCache& cache, size_t index, float x, float y) {
	auto& value = cache.texts[index];
	value.at = {x, y};
	cache.visibleTexts.push_back(index);
	for (const auto& piece : value.layout.pieces) {
		const auto& run = value.runs[piece.run];
		if (piece.ellipsis || run.style.href.empty())
			continue;
		cache.targets.push_back({{x + piece.x, y + static_cast<float>(piece.line) * value.layout.lineHeight,
		                          piece.width, value.layout.lineHeight},
		                         view::TargetKind::Link,
		                         run.style.href});
	}
}

void addLine(DataCache& cache, SDL_FPoint from, SDL_FPoint to, SDL_Color color, float thickness) {
	const float half = std::max(1.0f, thickness) * 0.5f;
	cache.lines.push_back({from,
	                       to,
	                       {std::min(from.x, to.x) - half, std::min(from.y, to.y) - half,
	                        std::abs(to.x - from.x) + half * 2, std::abs(to.y - from.y) + half * 2},
	                       color,
	                       thickness});
}

void syncKv(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	cache.inlineKv = node.str("layout") == "inline";
	const auto& items = node.prop("items");
	if (!items.is_array())
		return;
	if (cache.inlineKv) {
		text::Runs runs;
		for (const auto& item : items) {
			if (!runs.empty())
				runs.push_back({"  ·  ", style(text::palette::dim)});
			append(runs, text::toRuns(prop(item, "k"), style(text::palette::muted)));
			runs.push_back({" ", style()});
			append(runs, text::toRuns(prop(item, "v")));
		}
		const size_t index = addText(cache, engine, std::move(runs));
		cache.naturalWidth = cache.texts[index].naturalWidth + fonts.size() * gfx::space::sm * 2;
		return;
	}
	float valueWidth = 0;
	for (const auto& item : items) {
		const size_t key = addText(cache, engine, text::toRuns(prop(item, "k"), style(text::palette::muted)));
		const size_t value = addText(cache, engine, text::toRuns(prop(item, "v")));
		cache.keyWidth = std::max(cache.keyWidth, cache.texts[key].naturalWidth);
		valueWidth = std::max(valueWidth, cache.texts[value].naturalWidth);
	}
	if (!items.empty()) {
		cache.keyWidth += fonts.size() * gfx::space::sm;
		// The intrinsic grid must still honour the same 40% key-column cap.
		cache.naturalWidth =
		    fonts.size() * gfx::space::sm * 2 + std::max(cache.keyWidth + valueWidth, cache.keyWidth / 0.4f);
	}
}

void syncTable(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	const auto& cols = node.prop("cols");
	const auto& rows = node.prop("rows");
	if (!cols.is_array())
		return;
	cache.columns.reserve(cols.size());
	for (const auto& col : cols) {
		DataCache::Column column;
		column.id = str(col, "id");
		const auto headerStyle = style(text::palette::muted, text::FontId::Bold);
		const auto& head = prop(col, "head");
		column.head = addText(cache, engine,
		                      head.is_null() ? text::Runs{{column.id, headerStyle}} : text::toRuns(head, headerStyle));
		const auto align = str(col, "align", "start"), truncate = str(col, "truncate", "end");
		column.align = align == "end" ? 2 : align == "center" ? 1 : 0;
		column.truncate = truncate == "start"  ? text::Truncate::Start
		                : truncate == "middle" ? text::Truncate::Middle
		                                       : text::Truncate::End;
		column.priority = col.value("priority", 0.0);
		column.grow = std::max(0.0f, col.value("grow", 1.0f));
		column.naturalWidth = cache.texts[column.head].naturalWidth;
		if (rows.is_array())
			for (const auto& row : rows) {
				const auto& cells = row.at("cells");
				const auto cell = cells.find(column.id);
				const size_t index = addText(cache, engine, cell == cells.end() ? text::Runs{} : text::toRuns(*cell));
				column.cells.push_back(index);
				column.naturalWidth = std::max(column.naturalWidth, cache.texts[index].naturalWidth);
			}
		cache.naturalWidth += std::max(fonts.size() * 5, column.naturalWidth + fonts.size() * gfx::space::sm * 2);
		cache.dropOrder.push_back(cache.columns.size());
		cache.columns.push_back(std::move(column));
	}
	std::sort(cache.dropOrder.begin(), cache.dropOrder.end(), [&](size_t a, size_t b) {
		return cache.columns[a].priority == cache.columns[b].priority
		         ? a < b
		         : cache.columns[a].priority < cache.columns[b].priority;
	});
}

void treeNode(const json& node, DataCache& cache, text::Engine& engine, int depth) {
	DataCache::Fold row;
	row.item = str(node, "id");
	row.key = "tree/" + row.item;
	row.depth = depth;
	row.expanded = node.value("open", false);
	if (auto icon = gfx::iconGlyph(str(node, "icon")); !icon.empty())
		row.icon =
		    addText(cache, engine, text::Runs{{std::move(icon), style(text::palette::muted, text::FontId::Icons)}});
	row.text = addText(cache, engine, text::toRuns(prop(node, "label")));
	const auto children = node.find("children");
	row.branch = children != node.end() && children->is_array() && !children->empty();
	const size_t index = cache.folds.size();
	cache.folds.push_back(std::move(row));
	if (children != node.end() && children->is_array())
		for (const auto& child : *children)
			treeNode(child, cache, engine, depth + 1);
	cache.folds[index].end = cache.folds.size();
}

std::string jsonKey(std::string_view key) {
	std::string result;
	result.reserve(key.size());
	for (const char c : key) {
		if (c == '~')
			result += "~0";
		else if (c == '/')
			result += "~1";
		else
			result += c;
	}
	return result;
}

bool inlineJsonString(std::string_view value, std::string& result, size_t& chars) {
	result += '"';
	if (++chars > 60)
		return false;
	for (const unsigned char c : value) {
		const size_t before = result.size();
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (c < 0x20) {
				constexpr std::string_view hex = "0123456789abcdef";
				result += "\\u00";
				result += hex[c >> 4];
				result += hex[c & 15];
			} else
				result += static_cast<char>(c);
		}
		if ((c & 0xc0) != 0x80)
			chars += result.size() - before;
		if (chars > 60)
			return false;
	}
	result += '"';
	return ++chars <= 60;
}

bool inlineJson(const json& value, std::string& result, size_t& chars) {
	if (value.is_string())
		return inlineJsonString(value.get_ref<const std::string&>(), result, chars);
	if (!value.is_structured()) {
		const auto scalar = value.dump();
		result += scalar;
		chars += scalar.size();
		return chars <= 60;
	}
	result += value.is_array() ? '[' : '{';
	if (++chars > 60)
		return false;
	bool first = true;
	for (auto it = value.begin(); it != value.end(); ++it) {
		if (!first) {
			result += ',';
			++chars;
		}
		first = false;
		if (value.is_object()) {
			if (!inlineJsonString(it.key(), result, chars))
				return false;
			result += ':';
			++chars;
		}
		if (!inlineJson(it.value(), result, chars))
			return false;
	}
	result += value.is_array() ? ']' : '}';
	return ++chars <= 60;
}

void jsonNode(const json& value, std::string_view label, std::string path, DataCache& cache, text::Engine& engine,
              int depth, int openLevels, bool root = false) {
	DataCache::Fold row;
	row.key = std::move(path);
	row.depth = depth;
	row.branch = value.is_structured() && !value.empty();
	row.expanded = depth < openLevels;
	text::Runs runs;
	if (!root)
		runs.push_back({std::string(label) + ": ", style(text::palette::muted, text::FontId::Mono)});
	std::string summary;
	size_t inlineChars = 0;
	// Stop at the inline limit instead of serialising every large subtree at every depth.
	if (!value.is_structured())
		summary = value.dump();
	else if (!inlineJson(value, summary, inlineChars))
		summary =
		    value.is_array() ? "[" + std::to_string(value.size()) + "]" : "{" + std::to_string(value.size()) + "}";
	const auto color = value.is_string()  ? text::palette::success
	                 : value.is_number()  ? text::palette::info
	                 : value.is_boolean() ? text::palette::pending
	                 : value.is_null()    ? text::palette::dim
	                                      : text::palette::fg;
	runs.push_back({std::move(summary), style(color, text::FontId::Mono)});
	row.text = addText(cache, engine, std::move(runs));
	const size_t index = cache.folds.size();
	cache.folds.push_back(std::move(row));
	if (value.is_array()) {
		for (size_t i = 0; i < value.size(); ++i) {
			const auto key = std::to_string(i);
			jsonNode(value[i], key, cache.folds[index].key + "/" + key, cache, engine, depth + 1, openLevels);
		}
	} else if (value.is_object()) {
		for (auto it = value.begin(); it != value.end(); ++it)
			jsonNode(it.value(), it.key(), cache.folds[index].key + "/" + jsonKey(it.key()), cache, engine, depth + 1,
			         openLevels);
	}
	cache.folds[index].end = cache.folds.size();
}

void syncFolds(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	if (node.kind == tsp::Kind::Tree) {
		const auto& nodes = node.prop("nodes");
		if (nodes.is_array())
			for (const auto& child : nodes)
				treeNode(child, cache, engine, 0);
	} else {
		const auto& root = node.prop("value");
		const int openLevels = static_cast<int>(node.num("open"));
		if (root.is_object() && !root.empty()) {
			for (auto it = root.begin(); it != root.end(); ++it)
				jsonNode(it.value(), it.key(), "json/" + jsonKey(it.key()), cache, engine, 0, openLevels);
		} else if (root.is_array() && !root.empty()) {
			for (size_t i = 0; i < root.size(); ++i) {
				const auto key = std::to_string(i);
				jsonNode(root[i], key, "json/" + key, cache, engine, 0, openLevels);
			}
		} else
			jsonNode(root, {}, "json", cache, engine, 0, openLevels, true);
	}
	for (const auto& row : cache.folds)
		cache.naturalWidth =
		    std::max(cache.naturalWidth,
		             fonts.size() * gfx::space::sm * 2 + static_cast<float>(row.depth) * fonts.size() * 1.2f
		                 + fonts.size() + fonts.size() * gfx::space::sm
		                 + (row.icon ? cache.texts[*row.icon].naturalWidth + fonts.size() * gfx::space::sm : 0)
		                 + cache.texts[row.text].naturalWidth);
}

void syncChart(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	cache.heatmap = node.str("kind") == "heatmap";
	cache.spark = node.str("kind") == "spark";
	cache.fill = tone(node.str("token"), text::palette::accent);
	const auto size = node.str("size", "md");
	const float unit = fonts.size() / 15;
	cache.cellSize = (size == "sm" ? 8.0f : size == "lg" ? 14.0f : 11.0f) * unit;
	cache.plotHeight = (size == "sm" ? 3.0f : size == "lg" ? 8.0f : 5.0f) * fonts.lineHeight();
	if (cache.heatmap) {
		const auto& cells = node.prop("cells");
		const auto& tips = node.prop("tips");
		if (cells.is_array()) {
			cache.heatRows = cells.size();
			for (size_t r = 0; r < cells.size(); ++r) {
				cache.heatCols = std::max(cache.heatCols, cells[r].size());
				for (size_t c = 0; c < cells[r].size(); ++c) {
					DataCache::Cell cell;
					cell.row = r;
					cell.col = c;
					cell.color = text::palette::surface2;
					if (cells[r][c].is_number()) {
						cell.color = cache.fill;
						cell.color.a = static_cast<Uint8>(
						    std::lround(static_cast<float>(cell.color.a)
						                * (0.15f + 0.85f * std::clamp(cells[r][c].get<float>(), 0.0f, 1.0f))));
					}
					if (tips.is_array() && r < tips.size() && tips[r].is_array() && c < tips[r].size()
					    && tips[r][c].is_string())
						cell.tip = tips[r][c].get_ref<const std::string&>();
					cache.cells.push_back(std::move(cell));
				}
			}
		}
		const auto& rows = node.prop("rows");
		if (rows.is_array())
			for (size_t r = 0; r < std::min(rows.size(), cache.heatRows); ++r) {
				if (rows[r].get_ref<const std::string&>().empty())
					continue;
				const size_t index = addText(cache, engine, text::toRuns(rows[r], style(text::palette::muted)));
				cache.keyWidth = std::max(cache.keyWidth, cache.texts[index].naturalWidth);
				cache.rowLabels.push_back({r, index});
			}
		if (cache.keyWidth > 0)
			cache.keyWidth += fonts.size() * gfx::space::sm;
		const auto& cols = node.prop("cols");
		if (cols.is_array())
			for (const auto& col : cols) {
				const size_t index =
				    addText(cache, engine, text::toRuns(prop(col, "label"), style(text::palette::muted)));
				cache.colLabels.push_back({col.value("at", size_t{0}), index});
			}
		cache.naturalWidth = cache.keyWidth + static_cast<float>(cache.heatCols) * (cache.cellSize + unit * 2);
		for (const auto& label : cache.colLabels)
			cache.naturalWidth =
			    std::max(cache.naturalWidth, cache.keyWidth + static_cast<float>(label.at) * (cache.cellSize + unit * 2)
			                                     + cache.texts[label.text].naturalWidth);
	} else {
		cache.low = node.num("min");
		cache.high = node.num("max", cache.low);
		const bool fixedHigh = node.prop("max").is_number();
		const auto& series = node.prop("series");
		if (series.is_array())
			for (const auto& item : series) {
				DataCache::Sample sample;
				sample.value = item.value("value", 0.0);
				sample.color = tone(str(item, "tone"), cache.fill);
				const auto label = str(item, "label");
				if (!label.empty())
					sample.label =
					    addText(cache, engine, text::Runs{{std::string(label), style(text::palette::muted)}});
				sample.tip = str(item, "title", label);
				if (!fixedHigh)
					cache.high = std::max(cache.high, sample.value);
				cache.series.push_back(std::move(sample));
			}
		const auto& marks = node.prop("marks");
		if (marks.is_array())
			for (const auto& item : marks) {
				DataCache::Mark mark;
				mark.value = item.value("value", 0.0);
				mark.color = tone(str(item, "tone"), text::palette::warning);
				mark.tip = str(item, "title");
				if (!fixedHigh)
					cache.high = std::max(cache.high, mark.value);
				cache.marks.push_back(std::move(mark));
			}
		if (cache.high <= cache.low)
			cache.high = cache.low + 1;
		cache.naturalWidth = fonts.size() * 24;
	}
	if (!node.prop("summary").is_null())
		cache.summary = addText(cache, engine, text::toRuns(node.prop("summary"), style(text::palette::muted)));
}

void measureKv(DataCache& cache, text::Engine& engine, const text::Fonts& fonts, float width) {
	if (cache.inlineKv) {
		if (cache.texts.empty())
			return;
		const float pad = fonts.size() * gfx::space::sm, inset = std::min(pad, width * 0.5f);
		layoutText(cache.texts.front(), engine, width - inset * 2, text::Wrap::Word, text::Truncate::End, 0);
		placeText(cache, 0, inset, pad);
		cache.height = cache.texts.front().layout.height + pad * 2;
		cache.measuredWidth = cache.texts.front().layout.width + inset * 2;
		return;
	}
	const float pad = fonts.size() * gfx::space::sm;
	const float inset = std::min(pad, width * 0.5f), inner = std::max(0.0f, width - inset * 2);
	const float keyWidth = std::min(cache.keyWidth, inner * 0.4f);
	for (size_t i = 0; i < cache.texts.size(); i += 2) {
		auto& key = cache.texts[i];
		auto& value = cache.texts[i + 1];
		layoutText(key, engine, std::max(0.0f, keyWidth - pad), text::Wrap::None);
		layoutText(value, engine, inner - keyWidth, text::Wrap::Word, text::Truncate::End, 0);
		placeText(cache, i, inset, cache.height + pad);
		placeText(cache, i + 1, inset + keyWidth, cache.height + pad);
		cache.height += std::max({fonts.lineHeight(), key.layout.height, value.layout.height}) + pad * 2;
	}
}

void measureTable(DataCache& cache, text::Engine& engine, const text::Fonts& fonts, float width) {
	if (cache.columns.empty())
		return;
	const float unit = fonts.size() / 15, pad = fonts.size() * gfx::space::sm;
	const float minColumn = fonts.size() * 5;
	size_t count = cache.columns.size();
	for (auto& column : cache.columns)
		column.shown = true;
	for (const size_t index : cache.dropOrder) {
		if (count == 1 || static_cast<float>(count) * minColumn <= width)
			break;
		cache.columns[index].shown = false;
		--count;
	}
	float totalGrow = 0;
	for (const auto& column : cache.columns)
		if (column.shown)
			totalGrow += column.grow;
	const float minimum = std::min(minColumn, width / static_cast<float>(count));
	const float extra = std::max(0.0f, width - minimum * static_cast<float>(count));
	const size_t rowCount = cache.columns.front().cells.size() + 1;
	float y = 0;
	for (size_t row = 0; row < rowCount; ++row) {
		float x = 0, height = fonts.lineHeight() + pad * 2;
		for (const auto& column : cache.columns) {
			if (!column.shown)
				continue;
			const float share = totalGrow > 0 ? column.grow / totalGrow : 1.0f / static_cast<float>(count);
			const float columnWidth = minimum + extra * share;
			const float inner = std::max(0.0f, columnWidth - pad * 2);
			const size_t index = row == 0 ? column.head : column.cells[row - 1];
			auto& cell = cache.texts[index];
			layoutText(cell, engine, inner, text::Wrap::None, column.truncate);
			const float space = std::max(0.0f, inner - cell.layout.width);
			const float offset = column.align == 2 ? space : column.align == 1 ? space * 0.5f : 0;
			placeText(cache, index, x + std::min(pad, columnWidth * 0.5f) + offset, y + pad);
			height = std::max(height, cell.layout.height + pad * 2);
			x += columnWidth;
		}
		if (row == 0 || row % 2 == 0)
			cache.boxes.push_back(
			    {{0, y, width, height}, 0, row == 0 ? text::palette::surface2 : text::palette::surface});
		addLine(cache, {0, y + height}, {width, y + height}, text::palette::border, unit);
		y += height;
	}
	cache.height = y;
}

void measureFolds(DataCache& cache, text::Engine& engine, const text::Fonts& fonts, float width) {
	const float em = fonts.size(), unit = em / 15, indent = em * 1.2f, arrow = em;
	const float pad = em * gfx::space::sm, gap = em * gfx::space::sm;
	// Tool data shares code's filled block and text inset; standalone data keeps its compact gutter.
	const bool toolData = cache.toolOutput && (cache.kind == tsp::Kind::Json || cache.kind == tsp::Kind::Tree);
	const float rowPad = cache.kind == tsp::Kind::Json ? em * 0.1f : pad;
	const size_t frame = cache.boxes.size();
	if (toolData)
		cache.boxes.push_back({{}, em / 3, text::palette::surface2});
	if (toolData)
		cache.height += em * (gfx::space::sm + gfx::space::lg);
	for (size_t i = 0; i < cache.folds.size();) {
		const auto& row = cache.folds[i];
		const float inset = toolData ? 0 : std::min(pad, width * 0.5f);
		const float left = std::min(width - inset, inset + static_cast<float>(row.depth) * indent);
		const float end = std::max(left, width - inset);
		float textX = std::min(end, left + arrow + (toolData ? 0 : gap));
		float textHeight = fonts.lineHeight();
		if (row.icon) {
			auto& icon = cache.texts[*row.icon];
			layoutText(icon, engine, std::max(0.0f, end - textX), text::Wrap::None);
			placeText(cache, *row.icon, textX, cache.height + rowPad);
			textX = std::min(end, textX + icon.layout.width + gap);
			textHeight = std::max(textHeight, icon.layout.height);
		}
		auto& label = cache.texts[row.text];
		layoutText(label, engine, end - textX, text::Wrap::None);
		const float height = std::max(textHeight, label.layout.height) + rowPad * 2;
		placeText(cache, row.text, textX, cache.height + rowPad);
		if (cache.kind == tsp::Kind::Tree)
			cache.targets.push_back({{left, cache.height, width - left, height}, view::TargetKind::Select, row.item});
		if (row.branch) {
			const float centerX = left + arrow * 0.45f, centerY = cache.height + height * 0.5f, arm = arrow * 0.2f;
			if (row.expanded) {
				addLine(cache, {centerX - arm, centerY - arm * 0.5f}, {centerX, centerY + arm * 0.5f},
				        text::palette::muted, unit * 1.5f);
				addLine(cache, {centerX, centerY + arm * 0.5f}, {centerX + arm, centerY - arm * 0.5f},
				        text::palette::muted, unit * 1.5f);
			} else {
				addLine(cache, {centerX - arm * 0.5f, centerY - arm}, {centerX + arm * 0.5f, centerY},
				        text::palette::muted, unit * 1.5f);
				addLine(cache, {centerX + arm * 0.5f, centerY}, {centerX - arm * 0.5f, centerY + arm},
				        text::palette::muted, unit * 1.5f);
			}
			cache.targets.push_back(
			    {{left, cache.height,
			      cache.kind == tsp::Kind::Tree ? std::min(arrow + gap, width - left) : width - left, height},
			     view::TargetKind::Expand,
			     row.key});
		}
		cache.height += height;
		i = row.branch && !row.expanded ? row.end : i + 1;
	}
	if (toolData) {
		cache.height += em * gfx::space::lg;
		cache.boxes[frame].rect = {0, em * gfx::space::sm, width, cache.height - em * gfx::space::sm};
		cache.height += em * gfx::space::sm;
	}
}

void measureHeatmap(DataCache& cache, text::Engine& engine, const text::Fonts& fonts, float width) {
	const float unit = fonts.size() / 15, gapToLabel = fonts.size() * gfx::space::sm;
	const float labelWidth = std::min(cache.keyWidth, width * 0.4f);
	const float available = std::max(0.0f, width - labelWidth);
	const float columns = static_cast<float>(cache.heatCols);
	const float gap = columns > 1 ? std::min(unit * 2, available / columns * 0.2f) : 0;
	const float side =
	    columns > 0 ? std::max(0.0f, std::min(cache.cellSize, (available - (columns - 1) * gap) / columns)) : 0;
	const float step = side + gap;
	const float rowStep = cache.rowLabels.empty() ? step : std::max(step, fonts.lineHeight() + gapToLabel);
	const float top = cache.colLabels.empty() ? 0 : fonts.lineHeight() + gapToLabel;
	for (const auto& label : cache.colLabels) {
		const float x = labelWidth + static_cast<float>(label.at) * step;
		if (x >= width)
			continue;
		auto& value = cache.texts[label.text];
		layoutText(value, engine, width - x, text::Wrap::None);
		placeText(cache, label.text, x, 0);
	}
	for (const auto& label : cache.rowLabels) {
		auto& value = cache.texts[label.text];
		layoutText(value, engine, std::max(0.0f, labelWidth - gapToLabel), text::Wrap::None);
		placeText(cache, label.text, 0,
		          top + static_cast<float>(label.at) * rowStep + (rowStep - value.layout.height) * 0.5f);
	}
	for (const auto& cell : cache.cells) {
		const SDL_FRect rect{labelWidth + static_cast<float>(cell.col) * step,
		                     top + static_cast<float>(cell.row) * rowStep + (rowStep - side) * 0.5f, side, side};
		cache.boxes.push_back({rect, unit * 2, cell.color});
		if (!cell.tip.empty())
			cache.targets.push_back({rect, view::TargetKind::Hover, cell.tip});
	}
	cache.height = top + static_cast<float>(cache.heatRows) * rowStep;
}

void measureSeries(DataCache& cache, text::Engine& engine, const text::Fonts& fonts, float width) {
	const float unit = fonts.size() / 15, height = cache.plotHeight, labelGap = fonts.size() * gfx::space::sm;
	auto background = text::palette::surface2;
	background.a = 128;
	cache.boxes.push_back({{0, 0, width, height}, unit * 4, background});
	const float step = width / static_cast<float>(std::max(size_t{1}, cache.series.size()));
	auto yOf = [&](double value) {
		return height
		     * (1
		        - static_cast<float>((std::clamp(value, cache.low, cache.high) - cache.low)
		                             / (cache.high - cache.low)));
	};
	const float baseline = yOf(0);
	SDL_FPoint previous{};
	bool labels = false;
	for (size_t i = 0; i < cache.series.size(); ++i) {
		const auto& sample = cache.series[i];
		const float x = static_cast<float>(i) * step, y = yOf(sample.value);
		const SDL_FPoint point{x + step * 0.5f, y};
		if (cache.spark) {
			if (i > 0)
				addLine(cache, previous, point, cache.fill, unit * 2);
			else if (cache.series.size() == 1)
				cache.boxes.push_back(
				    {{point.x - unit * 2, point.y - unit * 2, unit * 4, unit * 4}, unit * 2, sample.color});
			previous = point;
		} else {
			const float gap = std::min(unit, step * 0.15f);
			cache.boxes.push_back(
			    {{x + gap, std::min(y, baseline), std::max(0.0f, step - gap * 2), std::abs(baseline - y)},
			     unit * 2,
			     sample.color});
		}
		if (!sample.tip.empty())
			cache.targets.push_back({{x, 0, step, height}, view::TargetKind::Hover, sample.tip});
		if (sample.label) {
			auto& label = cache.texts[*sample.label];
			layoutText(label, engine, std::max(0.0f, step - labelGap * 2), text::Wrap::None);
			placeText(cache, *sample.label, x + (step - label.layout.width) * 0.5f, height + labelGap);
			labels = true;
		}
	}
	for (const auto& mark : cache.marks) {
		const float y = yOf(mark.value);
		for (float x = 0; x < width; x += unit * 8)
			addLine(cache, {x, y}, {std::min(x + unit * 4, width), y}, mark.color, unit);
		if (!mark.tip.empty())
			cache.targets.push_back({{0, y - unit * 3, width, unit * 6}, view::TargetKind::Hover, mark.tip});
	}
	cache.height = height + (labels ? fonts.lineHeight() + labelGap : 0);
}

} // namespace

bool dataKind(tsp::Kind kind) {
	return kind == tsp::Kind::Kv || kind == tsp::Kind::Table || kind == tsp::Kind::Tree || kind == tsp::Kind::Json
	    || kind == tsp::Kind::Chart;
}

void syncData(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	if (cache.synced && cache.kind == node.kind && cache.revision == node.rev && cache.fontSize == fonts.size())
		return;
	// Only user-overridden folds survive prop or font updates; new rows use their wire defaults.
	std::unordered_map<std::string, bool> folded;
	for (auto& row : cache.folds)
		if (row.changed)
			folded.emplace(std::move(row.key), row.expanded);
	const bool toolOutput = cache.toolOutput;
	forgetData(cache);
	cache = DataCache{};
	cache.toolOutput = toolOutput;
	cache.kind = node.kind;
	cache.revision = node.rev;
	cache.fontSize = fonts.size();
	switch (node.kind) {
	case tsp::Kind::Kv:
		syncKv(node, cache, engine, fonts);
		break;
	case tsp::Kind::Table:
		syncTable(node, cache, engine, fonts);
		break;
	case tsp::Kind::Tree:
	case tsp::Kind::Json:
		syncFolds(node, cache, engine, fonts);
		break;
	case tsp::Kind::Chart:
		syncChart(node, cache, engine, fonts);
		break;
	default:
		break;
	}
	for (auto& row : cache.folds)
		if (const auto found = folded.find(row.key); found != folded.end()) {
			row.expanded = found->second;
			row.changed = true;
		}
	cache.synced = true;
}

SDL_FPoint measureData(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts,
                       float width) {
	syncData(node, cache, engine, fonts);
	width = std::max(0.0f, width);
	if (cache.width == width)
		return {cache.measuredWidth, cache.height};
	cache.width = width;
	if (width >= naturalLimit)
		width = cache.naturalWidth;
	cache.measuredWidth = width;
	cache.height = 0;
	cache.visibleTexts.clear();
	cache.boxes.clear();
	cache.lines.clear();
	cache.targets.clear();
	switch (node.kind) {
	case tsp::Kind::Kv:
		measureKv(cache, engine, fonts, width);
		break;
	case tsp::Kind::Table:
		measureTable(cache, engine, fonts, width);
		break;
	case tsp::Kind::Tree:
	case tsp::Kind::Json:
		measureFolds(cache, engine, fonts, width);
		break;
	case tsp::Kind::Chart:
		if (cache.heatmap)
			measureHeatmap(cache, engine, fonts, width);
		else
			measureSeries(cache, engine, fonts, width);
		if (cache.summary) {
			auto& summary = cache.texts[*cache.summary];
			layoutText(summary, engine, width, text::Wrap::Word, text::Truncate::End, 0);
			const float gap = cache.height > 0 && summary.layout.height > 0 ? fonts.size() * gfx::space::sm : 0;
			placeText(cache, *cache.summary, 0, cache.height + gap);
			cache.height += gap + summary.layout.height;
		}
		break;
	default:
		break;
	}
	return {cache.measuredWidth, cache.height};
}

bool paintData(const tsp::Node& node, DataCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, view::HitTargets& targets) {
	measureData(node, cache, engine, fonts, rect.w);
	gfx::ScopedClip clip(renderer, rect);
	if (clip.visible.w <= 0 || clip.visible.h <= 0)
		return false;
	auto absolute = [&](SDL_FRect local) {
		local.x += rect.x;
		local.y += rect.y;
		return local;
	};
	for (const auto& box : cache.boxes) {
		const auto bounds = absolute(box.rect);
		if (gfx::intersects(bounds, clip.visible))
			gfx::rounded(renderer, bounds, box.radius, box.color);
	}
	for (const auto& line : cache.lines) {
		if (!gfx::intersects(absolute(line.bounds), clip.visible))
			continue;
		gfx::line(renderer, {rect.x + line.from.x, rect.y + line.from.y}, {rect.x + line.to.x, rect.y + line.to.y},
		          line.color, line.thickness);
	}
	bool animating = false;
	for (const size_t index : cache.visibleTexts) {
		const auto& value = cache.texts[index];
		const float x = rect.x + value.at.x, y = rect.y + value.at.y;
		if (!gfx::intersects({x, y, value.layout.width, value.layout.height}, clip.visible))
			continue;
		engine.draw(value.runs, value.layout, x, y);
		if (value.animated && !animating)
			for (const auto& piece : value.layout.pieces) {
				if (value.runs[piece.run].style.fx != 0
				    && gfx::intersects({x + piece.x, y + static_cast<float>(piece.line) * value.layout.lineHeight,
				                        piece.width, value.layout.lineHeight},
				                       clip.visible)) {
					animating = true;
					break;
				}
			}
	}
	for (const auto& target : cache.targets)
		targets.add(gfx::intersect(absolute(target.rect), clip.visible), target.kind, target.item);
	return animating;
}

bool foldData(DataCache& cache, std::string_view item) {
	for (auto& row : cache.folds)
		if (row.branch && row.key == item) {
			row.expanded = !row.expanded;
			row.changed = true;
			cache.width = -1;
			return true;
		}
	return false;
}

void forgetData(DataCache& cache) {
	for (auto& value : cache.texts)
		value.layout = {};
	cache.visibleTexts.clear();
	cache.boxes.clear();
	cache.lines.clear();
	cache.targets.clear();
	cache.width = -1;
	cache.synced = false;
}

} // namespace kinds
