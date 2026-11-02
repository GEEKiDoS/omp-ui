#include "picker.h"

#include "core/text_util.h"
#include "gfx/draw.h"
#include "gfx/icons.h"
#include "inline.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <iterator>
#include <unordered_map>

namespace kinds {
namespace {

constexpr float natural = 1000000;
const json& field(const json& value, std::string_view key) {
	static const json absent;
	const auto found = value.find(key);
	return found == value.end() ? absent : *found;
}
std::string_view string(const json& value, std::string_view key, std::string_view fallback = {}) {
	const auto& item = field(value, key);
	return item.is_string() ? item.get_ref<const std::string&>() : fallback;
}
bool disabled(const json& value) {
	const auto& item = field(value, "disabled");
	return !item.is_null() && item != false;
}
SDL_Color tone(std::string_view value, SDL_Color fallback) {
	text::RunStyle style{.color = fallback};
	text::applyTokens(value, style);
	return style.color;
}
void append(text::Runs& runs, text::Runs extra) {
	runs.insert(runs.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));
}
void set(PickerText& value, text::Runs runs) {
	value.runs = std::move(runs);
	value.width = -1;
}
void line(PickerText& value, text::Engine& engine, float width = natural) {
	width = std::max(1.0f, width);
	if (value.width == width)
		return;
	value.layout = engine.layout(value.runs, {.width = width, .wrap = text::Wrap::None, .maxLines = 1});
	value.width = width;
}
text::Runs runs(const json& value, SDL_Color color = text::palette::fg, text::FontId font = text::FontId::Ui) {
	return text::toRuns(value, {.font = font, .color = color});
}
bool icon(text::Runs& value, std::string_view name, SDL_Color color) {
	if (auto glyph = gfx::iconGlyph(name); !glyph.empty()) {
		value.push_back({std::move(glyph), {.font = text::FontId::Icons, .color = color}});
		value.push_back({" ", {.color = color}});
		return true;
	}
	return false;
}
void avatar(text::Runs& runs, const json& mark) {
	if (icon(runs, string(mark, "icon"), text::palette::accent))
		return;
	std::string initials;
	const auto name = string(mark, "text");
	for (size_t i = 0; i < name.size() && initials.size() < 2; ++i)
		if (std::isalnum(static_cast<unsigned char>(name[i])) && (i == 0 || name[i - 1] == ' ' || name[i - 1] == '-'))
			initials.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(name[i]))));
	runs.push_back({initials + " ", {.font = text::FontId::Bold, .color = text::palette::accent}});
}
void buttonText(PickerButton& button, const json& value, text::Engine& engine, bool keys = false) {
	button.id = string(value, "id");
	button.disabled = disabled(value);
	button.primary = value.value("primary", false);
	button.danger = value.value("danger", false);
	button.end = value.value("end", false);
	button.active = value.value("on", false);
	const auto color = button.disabled                 ? text::palette::dim
	                 : button.danger                   ? text::palette::error
	                 : button.primary || button.active ? text::palette::accent
	                                                   : text::palette::muted;
	text::Runs label;
	icon(label, string(value, "icon"), color);
	append(label, runs(field(value, "label").is_null() ? json(button.id) : field(value, "label"), color));
	if (keys && field(value, "keys").is_array())
		for (const auto& key : value["keys"]) {
			if (!key.is_string())
				continue;
			PickerText caption;
			set(caption, runs(keyCap(key.get_ref<const std::string&>()), text::palette::dim, text::FontId::Mono));
			line(caption, engine);
			button.keys.push_back(std::move(caption));
		}
	if (field(value, "count").is_number()) {
		set(button.count,
		    runs(std::format("{:g}", value["count"].get<double>()), text::palette::dim, text::FontId::Mono));
		line(button.count, engine);
	}
	set(button.label, std::move(label));
	line(button.label, engine);
}
float buttonWidth(PickerButton& button, text::Engine& engine, float maximum, float em) {
	const float padding = em * gfx::space::md, gap = em * gfx::space::sm;
	float trailing = button.count.runs.empty() ? 0 : gap + button.count.layout.width;
	for (const auto& key : button.keys)
		trailing += gap + key.layout.width + padding * 2;
	line(button.label, engine, maximum - padding * 2 - trailing);
	return std::min(maximum, button.label.layout.width + trailing + padding * 2);
}
float buttonHeight(const PickerButton& button, float em) {
	const float inset = em * gfx::space::xs;
	float height = std::max(button.label.layout.height, button.count.layout.height);
	for (const auto& key : button.keys)
		height = std::max(height, key.layout.height + inset * 2);
	return height + inset * 2;
}
void labelRuns(const tsp::Node& node, const json& item, PickerRow& row) {
	const auto color = row.disabled ? text::palette::dim : tone(string(item, "tone"), text::palette::fg);
	const auto font = item.value("mono", false) ? text::FontId::Mono : text::FontId::Ui;
	auto& label = row.label.runs;
	if (field(item, "dot").is_string())
		label.push_back({"● ", {.color = tone(string(item, "dot"), text::palette::muted)}});
	const auto& mark = field(item, "mark");
	if (mark.is_object())
		avatar(label, mark);
	else
		icon(label, string(item, "icon", string(item, "role")), text::palette::muted);
	const auto& value = field(item, "label");
	const auto& matches = field(item, "hits").is_array() ? field(item, "hits") : field(node.prop("hits"), row.id);
	if (matches.is_array() && value.is_string()) {
		const auto& text = value.get_ref<const std::string&>();
		size_t position = 0;
		for (const auto& hit : matches) {
			if (!hit.is_array() || hit.size() < 2 || !hit[0].is_number_integer() || !hit[1].is_number_integer())
				continue;
			const size_t begin = text::byteOffset(text, hit[0].get<size_t>()),
			             end = text::byteOffset(text, hit[1].get<size_t>());
			if (begin < position || end <= begin)
				continue;
			label.push_back({text.substr(position, begin - position), {.font = font, .color = color}});
			label.push_back({text.substr(begin, end - begin),
			                 {.font = font, .color = text::palette::accent, .bg = text::palette::selected}});
			position = end;
		}
		label.push_back({text.substr(position), {.font = font, .color = color}});
	} else if (item.value("mono", false) && value.is_string()) {
		const auto& labelText = value.get_ref<const std::string&>();
		const size_t slash = labelText.rfind('/');
		if (slash != std::string::npos)
			label.push_back(
			    {labelText.substr(0, slash + 1), {.font = text::FontId::Mono, .color = text::palette::dim}});
		label.push_back({labelText.substr(slash == std::string::npos ? 0 : slash + 1),
		                 {.font = text::FontId::MonoBold, .color = color}});
	} else
		append(label, runs(value.is_null() ? json(row.id) : value, color, font));
	row.current =
	    node.prop("current").is_array()
	    && std::find(node.prop("current").begin(), node.prop("current").end(), row.id) != node.prop("current").end();
	if (field(item, "chips").is_array())
		for (const auto& chip : item["chips"]) {
			PickerExtra extra;
			extra.chip = true;
			extra.background = chip.value("on", false) ? text::palette::surface2 : SDL_Color{};
			const auto chipColor = chip.value("on", false) ? text::palette::accent : text::palette::dim;
			if (field(chip, "dot").is_string())
				extra.text.runs.push_back({"● ", {.color = tone(string(chip, "dot"), chipColor)}});
			extra.text.runs.push_back(
			    {std::string(string(chip, "text")) + (chip.value("auto", false) ? " ↻" : ""), {.color = chipColor}});
			row.extras.push_back(std::move(extra));
		}
	if (field(item, "badges").is_array())
		for (const auto& badge : item["badges"]) {
			PickerExtra extra;
			extra.text.runs = runs(string(badge, "text"), tone(string(badge, "tone"), text::palette::pending));
			row.extras.push_back(std::move(extra));
		}
	row.detail.runs = runs(field(item, "detail"), text::palette::muted);
	if (node.str("layout") != "cards" && !row.detail.runs.empty()) {
		PickerExtra extra;
		extra.text.runs = std::move(row.detail.runs);
		row.extras.push_back(std::move(extra));
		row.detail.runs.clear();
	}
}
std::string duration(double value) {
	const auto seconds = static_cast<int64_t>(std::max(0.0, value) / 1000);
	if (seconds >= 3600)
		return std::format("{}h {:02}m", seconds / 3600, seconds / 60 % 60);
	if (seconds >= 60)
		return std::format("{}m {:02}s", seconds / 60, seconds % 60);
	return std::format("{}s", seconds);
}
std::string formatNumber(double value) {
	if (std::abs(value) >= 1e6)
		return std::format("{:.1f}M", value / 1e6);
	if (std::abs(value) >= 1e3)
		return std::format("{:.1f}K", value / 1e3);
	return std::format("{:g}", value);
}
void facts(const tsp::Node& node, PickerRow& row, const PickerCache& cache, text::Engine& engine,
           const text::Fonts& fonts, float width) {
	row.facts.clear();
	const auto& item = node.prop("items")[row.item];
	const auto& values = field(item, "facts");
	const float em = fonts.size(), padding = em * gfx::space::md, gap = em * gfx::space::sm;
	float right = width - padding;
	for (size_t i = 0; i < cache.columns.size(); ++i) {
		const auto& column = cache.columns[i];
		if (!column.visible)
			continue;
		right = std::min(right, column.x - gap);
		const auto& value = field(values, column.id);
		if (value.is_null())
			continue;
		PickerFact fact;
		fact.x = column.x;
		fact.width = column.width;
		if (column.format == "bar" && value.is_number())
			fact.bar = std::clamp(value.get<float>(), 0.0f, 1.0f);
		else {
			if (value.is_number()) {
				double number = value.get<double>();
				if (column.format == "elapsed")
					number += static_cast<double>(SDL_GetTicks()) + row.clockOffset;
				const std::string valueText = column.format == "price" ? std::format("${:.2f}", number)
				                            : column.format == "time" || column.format == "elapsed"
				                                ? duration(number)
				                                : formatNumber(number);
				fact.text.runs = runs(valueText, text::palette::muted, text::FontId::Mono);
			} else
				fact.text.runs = runs(value, column.format == "dim" ? text::palette::dim : text::palette::muted);
			line(fact.text, engine, fact.width);
		}
		row.facts.push_back(std::move(fact));
	}
	const float depth = item.value("depth", 0.0f) * em;
	const bool tree = node.str("layout") == "tree", timeline = node.str("layout") == "timeline";
	const float left =
	    padding + depth + (row.current ? em * 0.45f + gap : 0) + (tree || timeline ? em * 0.6f + gap : 0);
	const float available = std::max(1.0f, right - left);
	line(row.label, engine);
	const float labelWidth = row.label.layout.width;
	if (item.value("mono", false) && labelWidth > available) {
		row.label.layout = engine.layout(
		    row.label.runs,
		    {.width = available, .wrap = text::Wrap::None, .truncate = text::Truncate::Start, .maxLines = 1});
		row.label.width = available;
	} else
		line(row.label, engine, available);
	float extraX = row.label.layout.width + gap;
	for (auto& extra : row.extras) {
		extra.x = -1;
		const float inset = extra.chip ? padding : 0;
		if (extraX + inset * 2 >= available)
			continue;
		line(extra.text, engine, available - extraX - inset * 2);
		extra.x = extraX;
		extra.width = extra.text.layout.width + inset * 2;
		extraX += extra.width + gap;
	}
	line(row.detail, engine, available);
	row.width = width;
	row.second = static_cast<int64_t>(SDL_GetTicks() / 1000);
}
SDL_FRect translated(SDL_FRect box, const SDL_FRect& origin) {
	box.x += origin.x;
	box.y += origin.y;
	return box;
}
bool contains(const SDL_FRect& box, float x, float y) {
	return x >= box.x && y >= box.y && x < box.x + box.w && y < box.y + box.h;
}
void ring(SDL_Renderer& renderer, const SDL_FRect& box, bool focused, float em) {
	if (focused && box.w > 0 && box.h > 0)
		gfx::roundedBorder(renderer, box, em * 0.4f, text::withAlpha(text::palette::accent, 145), em / 15);
}
void drawButton(PickerButton& button, text::Engine& engine, SDL_Renderer& renderer, const SDL_FRect& box, float em) {
	const float padding = em * gfx::space::md, inset = em * gfx::space::xs, gap = em * gfx::space::sm;
	if (button.active || button.primary || button.danger)
		gfx::rounded(renderer, box, em * 0.35f,
		             button.danger    ? text::withAlpha(text::palette::danger, 110)
		             : button.primary ? text::withAlpha(text::palette::primary, 170)
		                              : text::palette::surface2);
	gfx::ScopedClip clip(renderer, box);
	float x = box.x + padding;
	engine.draw(button.label.runs, button.label.layout, x, box.y + (box.h - button.label.layout.height) * 0.5f);
	x += button.label.layout.width;
	for (auto& key : button.keys) {
		x += gap;
		const SDL_FRect cap{x, box.y + (box.h - key.layout.height - inset * 2) * 0.5f, key.layout.width + padding * 2,
		                    key.layout.height + inset * 2};
		gfx::rounded(renderer, cap, em * 0.25f, text::palette::surface2);
		gfx::roundedBorder(renderer, cap, em * 0.25f, text::palette::border, em / 15);
		engine.draw(key.runs, key.layout, cap.x + padding, cap.y + inset);
		x += cap.w;
	}
	if (!button.count.runs.empty()) {
		x += gap;
		engine.draw(button.count.runs, button.count.layout, x, box.y + (box.h - button.count.layout.height) * 0.5f);
	}
}

} // namespace

void syncPicker(const tsp::Node& node, PickerCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	std::array<double, 8> stamps{};
	size_t stampIndex = 0;
	for (const auto name : {"items", "itemsAdd", "itemsDel", "order", "hits", "current", "layout", "columns"}) {
		const auto found = node.stamps.find(name);
		stamps[stampIndex++] = found == node.stamps.end() ? 0 : found->second;
	}
	if (!cache.ready || stamps != cache.catalogueStamps || cache.fontSize != fonts.size()) {
		cache.rows.clear();
		cache.catalogueStamps = stamps;
		cache.fontSize = fonts.size();
		cache.selected.clear();
		cache.shown = 0;
		std::unordered_map<std::string_view, size_t> byId;
		const auto& items = node.prop("items");
		if (items.is_array())
			for (size_t i = 0; i < items.size(); ++i)
				byId.emplace(string(items[i], "id"), i);
		auto itemRow = [&](size_t index) {
			const auto& item = items[index];
			PickerRow row;
			row.id = string(item, "id");
			row.item = index;
			row.disabled = disabled(item);
			row.clockOffset = node.since("items/" + row.id) - static_cast<double>(SDL_GetTicks());
			labelRuns(node, item, row);
			const bool detail = node.str("layout") == "cards" && !row.detail.runs.empty();
			row.height = fonts.lineHeight() * (detail ? 2 : 1) + fonts.size() * gfx::space::sm * (detail ? 3 : 2);
			cache.rows.push_back(std::move(row));
			++cache.shown;
		};
		if (node.prop("order").is_array())
			for (const auto& entry : node.prop("order")) {
				if (entry.is_string()) {
					const auto found = byId.find(entry.get_ref<const std::string&>());
					if (found != byId.end())
						itemRow(found->second);
				} else if (entry.is_object() && entry.contains("group")) {
					PickerRow row;
					row.group = true;
					row.height = fonts.lineHeight() + fonts.size() * gfx::space::sm * 2;
					row.label.runs =
					    runs(field(entry, "label").is_null() ? field(entry, "group") : field(entry, "label"),
					         text::palette::dim, text::FontId::Bold);
					if (field(entry, "count").is_number())
						row.label.runs.push_back({"  " + std::format("{:g}", entry["count"].get<double>()),
						                          {.font = text::FontId::Mono, .color = text::palette::dim}});
					cache.rows.push_back(std::move(row));
				}
			}
		else if (items.is_array())
			for (size_t i = 0; i < items.size(); ++i)
				itemRow(i);
		cache.contentHeight = 0;
		for (auto& row : cache.rows) {
			row.y = cache.contentHeight;
			cache.contentHeight += row.height;
		}
		cache.columns.clear();
		if (node.prop("columns").is_array())
			for (const auto& value : node.prop("columns")) {
				PickerColumn column;
				column.id = string(value, "id");
				column.format = string(value, "format", "text");
				column.priority = value.value("priority", 0);
				column.head.runs = runs(field(value, "head"), text::palette::dim);
				line(column.head, engine);
				column.width = std::max(column.head.layout.width, value.value("min", 0.0f) * fonts.monoAdvance());
				if (column.format == "bar")
					column.width = std::max(column.width, fonts.size() * 3.5f);
				else if (items.is_array())
					for (const auto& item : items) {
						const auto& fact = field(field(item, "facts"), column.id);
						if (fact.is_null())
							continue;
						const auto valueText =
						    fact.is_number() ? (column.format == "price" ? std::format("${:.2f}", fact.get<double>())
						                        : column.format == "time" || column.format == "elapsed"
						                            ? duration(fact.get<double>())
						                            : formatNumber(fact.get<double>()))
						                     : text::plainText(fact);
						column.width = std::max(
						    column.width,
						    std::min(fonts.size() * 8,
						             fonts.width(fact.is_number() ? text::FontId::Mono : text::FontId::Ui, valueText)));
					}
				cache.columns.push_back(std::move(column));
			}
		cache.ready = true;
	}
	auto heading = runs(node.prop("title"), text::palette::fg, text::FontId::Bold);
	text::Runs decorated;
	icon(decorated, node.str("icon"), text::palette::accent);
	append(decorated, std::move(heading));
	set(cache.title, std::move(decorated));
	set(cache.subtitle, runs(node.prop("subtitle"), text::palette::muted));
	std::string count = std::to_string(cache.shown);
	if (node.prop("total").is_number())
		count += " of " + std::to_string(static_cast<size_t>(node.num("total")));
	count += " " + std::string(node.str("noun", "items"));
	set(cache.count, runs(count, text::palette::dim));
	cache.hasQuery = node.prop("query").is_string();
	set(cache.query, runs(node.prop("query")));
	set(cache.placeholder, runs(node.str("placeholder", "Search"), text::palette::dim));
	set(cache.close, runs("×", text::palette::muted));
	const auto state = node.str("state", "ready");
	const auto& message = state == "ready" ? node.prop("empty") : node.prop("message");
	set(cache.message,
	    runs(message.is_null() ? json(state == "loading" ? "Loading…"
	                                  : state == "error" ? "Unable to load items"
	                                                     : "No " + std::string(node.str("noun", "items")))
	                           : message,
	         state == "error" ? text::palette::error : text::palette::muted));
	cache.tabs.clear();
	cache.scopes.clear();
	cache.strip.clear();
	cache.actions.clear();
	if (node.prop("tabs").is_array())
		for (const auto& value : node.prop("tabs")) {
			PickerButton button;
			buttonText(button, value, engine);
			button.active = button.id == node.str("tab");
			cache.tabs.push_back(std::move(button));
		}
	std::string previousGroup;
	if (node.prop("scopes").is_array())
		for (const auto& value : node.prop("scopes")) {
			const auto group = string(value, "group");
			if (!group.empty() && group != previousGroup) {
				PickerButton header;
				header.disabled = true;
				set(header.label, runs(group, text::palette::dim, text::FontId::Bold));
				line(header.label, engine);
				cache.scopes.push_back(std::move(header));
				previousGroup = group;
			}
			PickerButton button;
			buttonText(button, value, engine);
			button.active = button.id == node.str("scope");
			if (field(value, "dot").is_string())
				button.label.runs.insert(button.label.runs.begin(),
				                         {"● ", {.color = tone(string(value, "dot"), text::palette::muted)}});
			if (field(value, "mark").is_object()) {
				text::Runs mark;
				avatar(mark, value["mark"]);
				append(mark, std::move(button.label.runs));
				button.label.runs = std::move(mark);
			}
			button.label.width = -1;
			cache.scopes.push_back(std::move(button));
		}
	const auto& strip = node.prop("strip");
	set(cache.stripLabel, runs(field(strip, "label"), text::palette::muted));
	if (field(strip, "items").is_array())
		for (size_t i = 0; i < strip["items"].size(); ++i) {
			const auto& value = strip["items"][i];
			PickerButton button;
			buttonText(button, value, engine);
			button.value = std::to_string(i);
			button.active |= button.id == string(strip, "selected");
			if (field(value, "dot").is_string())
				button.label.runs.insert(button.label.runs.begin(),
				                         {"● ", {.color = tone(string(value, "dot"), text::palette::muted)}});
			button.label.width = -1;
			line(button.label, engine);
			cache.strip.push_back(std::move(button));
		}
	cache.confirmAct = string(node.prop("confirm"), "act", "confirm");
	set(cache.confirmText, runs(field(node.prop("confirm"), "text"), text::palette::warning));
	set(cache.confirmLabel,
	    runs(field(node.prop("confirm"), "label").is_null() ? json("Confirm") : field(node.prop("confirm"), "label"),
	         text::palette::warning));
	if (node.prop("actions").is_array())
		for (const auto& value : node.prop("actions")) {
			PickerButton button;
			buttonText(button, value, engine, true);
			cache.actions.push_back(std::move(button));
		}
	cache.width = cache.height = -1;
}

void layoutPicker(const tsp::Node& node, PickerCache& cache, text::Engine& engine, const text::Fonts& fonts,
                  float width, float height) {
	if (cache.width == width && cache.height == height && cache.children == node.children.size())
		return;
	cache.width = width;
	cache.height = height;
	cache.children = node.children.size();
	const float em = fonts.size(), pitch = fonts.lineHeight();
	const float pad = em * gfx::space::lg, padding = em * gfx::space::md, inset = em * gfx::space::xs,
	            gap = em * gfx::space::sm;
	const float inner = std::max(1.0f, width - pad * 2), controlHeight = pitch + inset * 2;
	float y = pad;
	line(cache.close, engine);
	line(cache.count, engine);
	const float closeWidth = cache.close.layout.width + padding * 2;
	cache.closeBox = {width - pad - closeWidth, y, closeWidth, controlHeight};
	line(cache.title, engine, inner - cache.count.layout.width - closeWidth - gap * 2);
	line(cache.subtitle, engine, inner);
	cache.head = {pad, y, inner, controlHeight + (cache.subtitle.runs.empty() ? 0 : pitch + gap)};
	y += cache.head.h + gap;
	cache.queryBox = {};
	if (cache.hasQuery) {
		cache.queryBox = {pad, y, inner, pitch + gap * 2};
		const auto query = node.str("query");
		cache.query.layout = engine.layout(
		    cache.query.runs,
		    {.width = std::max(natural, inner),
		     .wrap = text::Wrap::None,
		     .maxLines = 1,
		     .caret = text::byteOffset(
		         query, static_cast<size_t>(node.num("cursor", static_cast<double>(text::utf16Length(query)))))});
		line(cache.placeholder, engine, inner - padding * 2 - em * 0.75f - gap);
		y += cache.queryBox.h + gap;
	}
	cache.tabsBox = {};
	if (!cache.tabs.empty()) {
		float tabHeight = controlHeight;
		for (const auto& button : cache.tabs)
			tabHeight = std::max(tabHeight, buttonHeight(button, em));
		const float rowHeight = tabHeight + gap;
		float x = pad, tabY = y;
		for (auto& button : cache.tabs) {
			const float w = buttonWidth(button, engine, inner, em);
			if (x > pad && x + w > width - pad) {
				x = pad;
				tabY += rowHeight;
			}
			button.rect = {x, tabY, w, tabHeight};
			x += w + gap;
		}
		cache.tabsBox = {pad, y, inner, tabY - y + tabHeight};
		y += cache.tabsBox.h + gap;
	}
	float bottom = height - pad;
	cache.actionBox = {};
	if (!cache.actions.empty()) {
		float actionHeight = controlHeight;
		for (const auto& button : cache.actions)
			actionHeight = std::max(actionHeight, buttonHeight(button, em));
		const float rowHeight = actionHeight + gap;
		float used = 0;
		int rows = 1;
		for (auto& button : cache.actions) {
			const float w = buttonWidth(button, engine, inner, em);
			if (used > 0 && used + w > inner) {
				++rows;
				used = 0;
			}
			button.rect = {pad + used, static_cast<float>(rows - 1) * rowHeight, w, actionHeight};
			used += w + gap;
		}
		const float lastRow = static_cast<float>(rows - 1) * rowHeight;
		bottom -= static_cast<float>(rows) * rowHeight;
		cache.actionBox = {pad, bottom, inner, static_cast<float>(rows) * rowHeight};
		float endWidth = 0;
		for (const auto& button : cache.actions)
			if (button.end && button.rect.y == lastRow)
				endWidth += button.rect.w + gap;
		float right = width - pad - endWidth + gap;
		for (auto& button : cache.actions) {
			if (button.end && button.rect.y == lastRow) {
				button.rect.x = right;
				right += button.rect.w + gap;
			}
			button.rect.y += bottom + gap;
		}
		bottom -= gap;
	}
	cache.confirmBox = {};
	cache.confirmButton = {};
	if (node.prop("confirm").is_object()) {
		bottom -= controlHeight;
		cache.confirmBox = {pad, bottom, inner, controlHeight};
		line(cache.confirmLabel, engine, inner - padding * 2);
		const float confirmWidth = std::min(inner, cache.confirmLabel.layout.width + padding * 2);
		cache.confirmButton = {width - pad - confirmWidth, bottom, confirmWidth, controlHeight};
		line(cache.confirmText, engine, inner - confirmWidth - gap);
		bottom -= gap;
	}
	cache.stripBox = {};
	cache.stripContent = {};
	if (node.prop("strip").is_object()) {
		const float contentWidth = std::max(1.0f, inner - padding * 2), left = pad + padding;
		line(cache.stripLabel, engine, contentWidth * 0.3f);
		float stripHeight = std::max(controlHeight, cache.stripLabel.layout.height + inset * 2);
		for (const auto& button : cache.strip)
			stripHeight = std::max(stripHeight, buttonHeight(button, em));
		const float rowHeight = stripHeight + gap;
		float x = left + (cache.stripLabel.runs.empty() ? 0 : cache.stripLabel.layout.width + gap), stripY = 0;
		for (auto& button : cache.strip) {
			const float w = buttonWidth(button, engine, contentWidth, em);
			if (x > left && x + w > left + contentWidth) {
				x = left;
				stripY += rowHeight;
			}
			button.rect = {x, stripY, w, stripHeight};
			x += w + gap;
		}
		const float contentHeight = stripY + stripHeight;
		bottom -= contentHeight + gap * 2;
		cache.stripBox = {pad, bottom, inner, contentHeight + gap * 2};
		cache.stripContent = {left, bottom + gap, contentWidth, contentHeight};
		for (auto& button : cache.strip)
			button.rect.y += cache.stripContent.y;
		bottom -= gap;
	}
	const float bodyH = std::max(0.0f, bottom - y);
	const bool preview = !node.children.empty() && node.str("preview", "side") != "none";
	cache.side = preview && node.str("preview", "side") == "side" && inner > em * 54;
	cache.minimumHeight =
	    y + (height - bottom) + pitch * (preview && !cache.side ? 7 : 4) + (cache.columns.empty() ? 0 : controlHeight);
	const float scopeW = cache.scopes.empty() ? 0 : std::min(inner * 0.23f, em * 13);
	const float previewW = cache.side ? inner * 0.36f : 0;
	cache.scopesBox = {pad, y, scopeW, bodyH};
	cache.previewBox = {};
	cache.listBox = {pad + (scopeW > 0 ? scopeW + gap : 0), y,
	                 std::max(1.0f, inner - (scopeW > 0 ? scopeW + gap : 0) - (cache.side ? previewW + gap : 0)),
	                 bodyH};
	if (cache.side)
		cache.previewBox = {width - pad - previewW, y, previewW, bodyH};
	else if (preview) {
		const float previewH = std::min(bodyH * 0.35f, pitch * 8);
		cache.previewBox = {cache.listBox.x, y + bodyH - previewH, cache.listBox.w, previewH};
		cache.listBox.h = std::max(0.0f, cache.listBox.h - previewH - gap);
	}
	cache.columnsBox = {};
	if (!cache.columns.empty()) {
		cache.columnsBox = {cache.listBox.x, cache.listBox.y, cache.listBox.w,
		                    std::min(cache.listBox.h, controlHeight)};
		cache.listBox.y += cache.columnsBox.h;
		cache.listBox.h = std::max(0.0f, cache.listBox.h - cache.columnsBox.h);
		float available = cache.listBox.w * 0.55f, required = 0;
		for (auto& column : cache.columns) {
			column.visible = true;
			required += column.width + gap;
		}
		while (required > available) {
			auto hide = cache.columns.end();
			for (auto it = cache.columns.begin(); it != cache.columns.end(); ++it)
				if (it->visible && (hide == cache.columns.end() || it->priority < hide->priority))
					hide = it;
			if (hide == cache.columns.end())
				break;
			hide->visible = false;
			required -= hide->width + gap;
		}
		float right = cache.listBox.w - padding - (cache.contentHeight > cache.listBox.h ? gap : 0);
		for (auto it = cache.columns.rbegin(); it != cache.columns.rend(); ++it)
			if (it->visible) {
				it->x = right - it->width;
				right = it->x - gap;
			}
		for (auto& row : cache.rows)
			row.width = -1;
	}
	cache.scopesHeight = 0;
	for (auto& button : cache.scopes) {
		buttonWidth(button, engine, scopeW, em);
		button.rect = {pad, y + cache.scopesHeight, scopeW, std::max(controlHeight, buttonHeight(button, em))};
		cache.scopesHeight += button.rect.h + gap;
	}
	if (!cache.scopes.empty())
		cache.scopesHeight -= gap;
	cache.scopesMax = std::max(0.0f, cache.scopesHeight - bodyH);
	cache.scopesScroll = std::clamp(cache.scopesScroll, 0.0f, cache.scopesMax);
	cache.scrollMax = std::max(0.0f, cache.contentHeight - cache.listBox.h);
	cache.selected = node.str("selected");
	for (const auto& row : cache.rows)
		if (!row.group && row.id == cache.selected) {
			if (row.y < cache.scroll)
				cache.scroll = row.y;
			else if (row.y + row.height > cache.scroll + cache.listBox.h)
				cache.scroll = row.y + row.height - cache.listBox.h;
			break;
		}
	cache.scroll = std::clamp(cache.scroll, 0.0f, cache.scrollMax);
	const float messageLeft = padding + (node.str("state") == "loading" ? em * 0.8f + gap : 0);
	line(cache.message, engine, cache.listBox.w - messageLeft - padding);
}

bool paintPicker(const tsp::Node& node, PickerCache& cache, text::Engine& engine, const text::Fonts& fonts,
                 SDL_Renderer& renderer, const SDL_FRect& rect, bool focused, view::HitTargets& hits) {
	layoutPicker(node, cache, engine, fonts, rect.w, rect.h);
	bool columnsGrew = false;
	const auto clockSecond = static_cast<int64_t>(SDL_GetTicks() / 1000);
	const auto firstClock = std::lower_bound(cache.rows.begin(), cache.rows.end(), cache.scroll,
	                                         [](const PickerRow& row, float top) { return row.y + row.height <= top; });
	for (auto it = firstClock; it != cache.rows.end() && it->y < cache.scroll + cache.listBox.h; ++it) {
		const auto& row = *it;
		if (row.group || row.second == clockSecond)
			continue;
		for (auto& column : cache.columns)
			if (column.format == "elapsed") {
				const auto& value = field(field(node.prop("items")[row.item], "facts"), column.id);
				if (!value.is_number())
					continue;
				const float width =
				    fonts.width(text::FontId::Mono,
				                duration(value.get<double>() + static_cast<double>(SDL_GetTicks()) + row.clockOffset));
				if (width > column.width) {
					column.width = width;
					columnsGrew = true;
				}
			}
	}
	if (columnsGrew) {
		cache.width = -1;
		layoutPicker(node, cache, engine, fonts, rect.w, rect.h);
	}
	cache.painted = rect;
	const float em = fonts.size(), pitch = fonts.lineHeight(), unit = em / 15;
	const float padding = em * gfx::space::md, inset = em * gfx::space::xs, gap = em * gfx::space::sm;
	const auto focus = node.str("focus", "list");
	gfx::rounded(renderer, rect, em * 0.8f, text::palette::surface);
	const auto head = translated(cache.head, rect);
	const auto close = translated(cache.closeBox, rect);
	engine.draw(cache.title.runs, cache.title.layout, head.x, head.y + inset);
	engine.draw(cache.count.runs, cache.count.layout, close.x - gap - cache.count.layout.width, head.y + inset);
	engine.draw(cache.close.runs, cache.close.layout, close.x + padding, close.y + inset);
	hits.add(close, view::TargetKind::Action, "close");
	if (!cache.subtitle.runs.empty())
		engine.draw(cache.subtitle.runs, cache.subtitle.layout, head.x, head.y + pitch + inset * 2 + gap);
	cache.caret.reset();
	if (cache.hasQuery) {
		const auto box = translated(cache.queryBox, rect);
		gfx::rounded(renderer, box, em * 0.4f, text::palette::bg);
		gfx::roundedBorder(renderer, box, em * 0.4f, focused ? text::palette::focus : text::palette::border, unit);
		gfx::arc(renderer, {box.x + padding + em * 0.25f, box.y + box.h * 0.45f}, em * 0.25f, 0, 6.283185f,
		         text::palette::dim, unit * 1.5f);
		gfx::line(renderer, {box.x + padding + em * 0.43f, box.y + box.h * 0.55f},
		          {box.x + padding + em * 0.7f, box.y + box.h * 0.72f}, text::palette::dim, unit * 1.5f);
		const float textLeft = padding + em * 0.75f + gap;
		const SDL_FRect content{box.x + textLeft, box.y + gap, std::max(1.0f, box.w - textLeft - padding), pitch};
		gfx::ScopedClip clip(renderer, gfx::intersect(box, content));
		const float caretX = cache.query.layout.caret ? cache.query.layout.caret->x : 0;
		const float scroll = std::max(0.0f, caretX - content.w + unit * 1.6f);
		const float x = content.x - scroll, y = content.y;
		if (node.str("query").empty())
			engine.draw(cache.placeholder.runs, cache.placeholder.layout, x, y);
		else
			engine.draw(cache.query.runs, cache.query.layout, x, y);
		if (focused) {
			cache.caret = SDL_FRect{x + caretX, y + unit * 2, unit * 1.6f, pitch - unit * 4};
			gfx::fill(renderer, *cache.caret, text::palette::accent);
		}
	}
	for (auto& button : cache.tabs) {
		const auto box = translated(button.rect, rect);
		drawButton(button, engine, renderer, box, em);
		if (button.active)
			gfx::fill(renderer, {box.x, box.y + box.h - unit * 2, box.w, unit * 2}, text::palette::accent);
		if (!button.disabled)
			hits.add(box, view::TargetKind::Action, "tab", 0, button.id);
	}
	ring(renderer, translated(cache.tabsBox, rect), focused && focus == "tabs", em);
	if (!cache.scopes.empty()) {
		const auto box = translated(cache.scopesBox, rect);
		gfx::ScopedClip clip(renderer, box);
		for (auto& button : cache.scopes) {
			auto hit = translated(button.rect, rect);
			hit.y -= cache.scopesScroll;
			if (!gfx::intersects(hit, box))
				continue;
			drawButton(button, engine, renderer, hit, em);
			if (!button.id.empty() && !button.disabled)
				hits.add(gfx::intersect(hit, box), view::TargetKind::Action, "scope", 0, button.id);
		}
		ring(renderer, box, focused && focus == "scopes", em);
	}
	for (const auto& column : cache.columns)
		if (column.visible)
			engine.draw(column.head.runs, column.head.layout,
			            rect.x + cache.columnsBox.x + column.x + column.width - column.head.layout.width,
			            rect.y + cache.columnsBox.y + inset);
	const auto list = translated(cache.listBox, rect);
	bool elapsed = false;
	{
		gfx::ScopedClip clip(renderer, list);
		if (node.str("state", "ready") != "ready" || cache.shown == 0) {
			const bool loading = node.str("state") == "loading";
			engine.draw(cache.message.runs, cache.message.layout, list.x + padding + (loading ? em * 0.8f + gap : 0),
			            list.y + inset);
			if (loading) {
				const float start = static_cast<float>(SDL_GetTicks() % 1000) / 1000 * 6.283185f;
				gfx::arc(renderer, {list.x + padding + em * 0.4f, list.y + inset + pitch * 0.5f}, em * 0.4f, start,
				         start + 4.5f, text::palette::accent, unit * 2);
			}
		}
		const float listWidth = list.w - (cache.scrollMax > 0 ? gap : 0);
		const auto first = std::lower_bound(cache.rows.begin(), cache.rows.end(), cache.scroll,
		                                    [](const PickerRow& row, float top) { return row.y + row.height <= top; });
		for (auto it = first;
		     node.str("state", "ready") == "ready" && it != cache.rows.end() && it->y < cache.scroll + list.h; ++it) {
			auto& row = *it;
			const SDL_FRect box{list.x, list.y + row.y - cache.scroll, listWidth, row.height};
			if (row.group) {
				line(row.label, engine, listWidth - padding * 2);
				engine.draw(row.label.runs, row.label.layout, box.x + padding, box.y + gap);
				continue;
			}
			const auto& item = node.prop("items")[row.item];
			const auto& columns = node.prop("columns");
			bool clock = false;
			if (columns.is_array())
				for (const auto& column : columns)
					clock |= string(column, "format") == "elapsed"
					      && field(field(item, "facts"), string(column, "id")).is_number();
			elapsed |= clock;
			if (row.width != listWidth || (clock && row.second != static_cast<int64_t>(SDL_GetTicks() / 1000)))
				facts(node, row, cache, engine, fonts, listWidth);
			if (row.id == node.str("selected"))
				gfx::rounded(renderer, box, em * 0.4f,
				             focused && focus == "list" ? text::palette::selected : text::palette::surface2);
			const bool tree = node.str("layout") == "tree", timeline = node.str("layout") == "timeline";
			const float depth = item.value("depth", 0.0f) * em;
			const float y = box.y + gap;
			float left = box.x + padding + depth;
			if (row.current) {
				gfx::rounded(renderer, {left, y + pitch * 0.5f - em * 0.225f, em * 0.45f, em * 0.45f}, em * 0.225f,
				             text::palette::success);
				left += em * 0.45f + gap;
			}
			if (tree) {
				const float cx = left + em * 0.3f, cy = box.y + row.height * 0.5f;
				if (item.value("open", false)) {
					gfx::line(renderer, {cx - em * 0.2f, cy - em * 0.1f}, {cx, cy + em * 0.1f}, text::palette::dim,
					          unit);
					gfx::line(renderer, {cx, cy + em * 0.1f}, {cx + em * 0.2f, cy - em * 0.1f}, text::palette::dim,
					          unit);
				} else {
					gfx::line(renderer, {cx - em * 0.1f, cy - em * 0.2f}, {cx + em * 0.1f, cy}, text::palette::dim,
					          unit);
					gfx::line(renderer, {cx + em * 0.1f, cy}, {cx - em * 0.1f, cy + em * 0.2f}, text::palette::dim,
					          unit);
				}
			}
			if (timeline) {
				const auto color = string(item, "node") == "user" ? text::palette::accent
				                 : string(item, "node") == "tool" ? text::palette::pending
				                                                  : text::palette::dim;
				gfx::line(renderer, {left + em * 0.3f, box.y}, {left + em * 0.3f, box.y + box.h}, text::palette::border,
				          unit);
				gfx::rounded(renderer, {left + em * 0.1f, y + pitch * 0.5f - em * 0.2f, em * 0.4f, em * 0.4f},
				             em * 0.2f, color);
			}
			const float x = left + (tree || timeline ? em * 0.6f + gap : 0);
			engine.draw(row.label.runs, row.label.layout, x, y);
			for (auto& extra : row.extras)
				if (extra.x >= 0) {
					const SDL_FRect chip{x + extra.x, y - inset, extra.width, extra.text.layout.height + inset * 2};
					if (extra.background.a)
						gfx::rounded(renderer, chip, em * 0.3f, extra.background);
					engine.draw(extra.text.runs, extra.text.layout, chip.x + (extra.chip ? padding : 0), y);
				}
			if (!row.detail.runs.empty())
				engine.draw(row.detail.runs, row.detail.layout, x, y + pitch + gap);
			for (auto& fact : row.facts) {
				if (fact.bar >= 0) {
					const SDL_FRect bar{box.x + fact.x, y + pitch * 0.45f, fact.width, em * 0.3f};
					gfx::rounded(renderer, bar, em * 0.15f, text::palette::surface2);
					gfx::rounded(renderer, {bar.x, bar.y, bar.w * fact.bar, bar.h}, em * 0.15f, text::palette::accent);
				} else
					engine.draw(fact.text.runs, fact.text.layout, box.x + fact.x + fact.width - fact.text.layout.width,
					            y);
			}
			if (!row.disabled)
				hits.add(gfx::intersect(box, list), view::TargetKind::Select, row.id);
			if (row.disabled) {
				const auto& reason =
				    field(item, "disabled").is_string() ? field(item, "disabled") : field(item, "title");
				if (reason.is_string())
					hits.add(gfx::intersect(box, list), view::TargetKind::Hover, reason.get_ref<const std::string&>());
			}
		}
	}
	ring(renderer, list, focused && focus == "list", em);
	if (cache.scrollMax > 0) {
		const float thumb = std::max(em, list.h * list.h / cache.contentHeight);
		gfx::rounded(
		    renderer,
		    {list.x + list.w - unit * 3, list.y + (list.h - thumb) * cache.scroll / cache.scrollMax, unit * 2, thumb},
		    unit, text::palette::dim);
	}
	ring(renderer, translated(cache.previewBox, rect), focused && focus == "preview", em);
	if (cache.stripBox.h > 0) {
		const auto box = translated(cache.stripBox, rect), content = translated(cache.stripContent, rect);
		{
			gfx::ScopedClip clip(renderer, content);
			const float rowHeight = cache.strip.empty() ? content.h : cache.strip.front().rect.h;
			engine.draw(cache.stripLabel.runs, cache.stripLabel.layout, content.x,
			            content.y + (rowHeight - cache.stripLabel.layout.height) * 0.5f);
			for (auto& button : cache.strip) {
				const auto hit = translated(button.rect, rect);
				drawButton(button, engine, renderer, hit, em);
				hits.add(gfx::intersect(hit, content), view::TargetKind::Action, "strip", 0, button.value);
			}
		}
		ring(renderer, box, focused && focus == "strip", em);
	}
	if (cache.confirmBox.h > 0) {
		const auto box = translated(cache.confirmBox, rect), button = translated(cache.confirmButton, rect);
		engine.draw(cache.confirmText.runs, cache.confirmText.layout, box.x, box.y + inset);
		gfx::rounded(renderer, button, em * 0.35f, text::withAlpha(text::palette::caution, 130));
		engine.draw(cache.confirmLabel.runs, cache.confirmLabel.layout, button.x + padding, button.y + inset);
		hits.add(button, view::TargetKind::Action, cache.confirmAct);
	}
	if (cache.actionBox.h > 0) {
		const auto box = translated(cache.actionBox, rect);
		gfx::line(renderer, {box.x, box.y}, {box.x + box.w, box.y}, text::palette::border, unit);
		for (auto& button : cache.actions) {
			const auto hit = translated(button.rect, rect);
			drawButton(button, engine, renderer, hit, em);
			if (!button.disabled)
				hits.add(hit, view::TargetKind::Action, button.id);
		}
	}
	cache.nextWake = elapsed ? 1000 - static_cast<int>(SDL_GetTicks() % 1000) : -1;
	return node.str("state") == "loading";
}

bool scrollPicker(PickerCache& cache, float x, float y, float delta) {
	if (contains(translated(cache.listBox, cache.painted), x, y)) {
		cache.scroll = std::clamp(cache.scroll + delta, 0.0f, cache.scrollMax);
		return true;
	}
	if (contains(translated(cache.scopesBox, cache.painted), x, y)) {
		cache.scopesScroll = std::clamp(cache.scopesScroll + delta, 0.0f, cache.scopesMax);
		return true;
	}
	if (contains(translated(cache.previewBox, cache.painted), x, y)) {
		cache.previewScroll = std::clamp(cache.previewScroll + delta, 0.0f, cache.previewMax);
		return true;
	}
	return false;
}

} // namespace kinds
