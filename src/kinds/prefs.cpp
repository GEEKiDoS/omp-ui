#include "prefs.h"
#include "inline.h"

#include "core/text_util.h"
#include "gfx/draw.h"
#include "gfx/icons.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace kinds {
namespace {

constexpr SDL_Color sheet{27, 37, 49, 255}, card{29, 32, 40, 255};

std::string stringValue(const json& value) {
	return value.is_string() ? value.get<std::string>() : value.is_null() ? std::string{} : value.dump();
}
const json& array(const json& object, std::string_view key) {
	static const json empty = json::array();
	const auto found = object.find(key);
	return found != object.end() && found->is_array() ? *found : empty;
}
bool disabled(const json& value) {
	const auto found = value.find("disabled");
	return found != value.end() && !found->is_null() && *found != false;
}
SDL_FRect moved(SDL_FRect rect, float x, float y) {
	rect.x += x;
	rect.y += y;
	return rect;
}
bool contains(const SDL_FRect& rect, float x, float y) {
	return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

class Builder {
public:
	Builder(PrefsCache& cache, text::Engine& engine, const text::Fonts& fonts)
	    : cache(cache), engine(engine), fonts(fonts), em(fonts.size()), unit(em / 15), pitch(fonts.lineHeight()) {}
	float label(const json& value, SDL_FRect rect, SDL_Color color = text::palette::fg,
	            text::FontId font = text::FontId::Ui, bool body = true, int lines = 0,
	            std::optional<size_t> caret = {}) {
		PrefsText item;
		item.runs = text::toRuns(value, {.font = font, .color = color});
		item.layout = engine.layout(item.runs, {.width = std::max(1.0f, rect.w),
		                                        .wrap = lines == 1 ? text::Wrap::None : text::Wrap::Word,
		                                        .maxLines = lines,
		                                        .caret = caret});
		item.rect = rect;
		item.body = body;
		item.popup = popupMode;
		const float height = item.layout.height;
		if (caret && item.layout.caret) {
			cache.caret = SDL_FRect{rect.x + item.layout.caret->x, rect.y + item.layout.caret->y + unit * 2, unit * 2,
			                        pitch - unit * 4};
			cache.caretBody = body;
		}
		cache.texts.push_back(std::move(item));
		return height;
	}
	void shape(SDL_FRect rect, SDL_Color color, float radius = 0, bool border = false, bool body = true) {
		cache.shapes.push_back({rect, color, radius, border, body, popupMode});
	}
	void target(SDL_FRect rect, view::TargetKind kind, std::string item = {}, std::optional<json> value = {},
	            bool body = true) {
		cache.targets.push_back({rect, kind, std::move(item), std::move(value), body, popupMode});
	}
	void button(const json& value, SDL_FRect rect, bool on, bool off, view::TargetKind kind, const std::string& item,
	            std::optional<json> payload = {}, text::FontId font = text::FontId::Ui) {
		auto background = on ? text::palette::accent : text::palette::surface2;
		if (on)
			background.a = 48;
		shape(rect, background, em * .45f);
		shape(rect, on ? text::palette::accent : text::palette::border, em * .45f, true);
		const auto content = stringValue(value);
		const float textWidth = std::min(std::max(1.0f, rect.w - em * gfx::space::md * 2), fonts.width(font, content));
		label(value, {rect.x + (rect.w - textWidth) * .5f, rect.y + (rect.h - pitch) * .5f, textWidth, pitch},
		      off  ? text::palette::dim
		      : on ? text::palette::accent
		           : text::palette::fg,
		      font, true, 1);
		if (!off)
			target(rect, kind, item, std::move(payload));
	}
	float controlWidth(const json& control, float available) const {
		const float maximum = std::min(available * .46f, em * 13.5f);
		const float padding = em * gfx::space::md, gap = em * gfx::space::sm;
		const auto kind = control.value("k", "");
		if (kind == "switch")
			return em * 2.25f;
		if (kind == "choice") {
			const auto& options = array(control, "options");
			const auto mode = control.value("style", "auto");
			if (mode != "segmented" && !(mode == "auto" && options.size() <= 3)) {
				auto value = stringValue(control.value("value", json{}));
				for (const auto& option : options)
					if (option.at("value") == control.value("value", json{}))
						value = option.value("label", value);
				return std::min(maximum,
				                fonts.width(control.value("mono", false) ? text::FontId::Mono : text::FontId::Ui, value)
				                    + padding * 2 + gap + em);
			}
			const auto font = control.value("mono", false) ? text::FontId::Mono : text::FontId::Ui;
			float width = 0;
			for (const auto& option : options)
				width += fonts.width(font, option.value("label", stringValue(option.at("value")))) + padding * 2;
			if (!options.empty())
				width += gap * static_cast<float>(options.size() - 1);
			return std::min(available * .55f, width);
		}
		if (kind == "action")
			return std::min(maximum, fonts.width(text::FontId::Ui, control.value("label", "Run")) + padding * 2);
		return maximum;
	}
	// The open choice's options as a floating menu at `anchor` (see PrefsPopup): as wide as the
	// widest option (at least the pill, at most the sheet), each option's detail wrapped under it.
	void menu(const json& options, const json& selected, const std::string& id, text::FontId font, bool off,
	          SDL_FRect anchor) {
		const float padding = em * gfx::space::md, inset = em * gfx::space::xs, gap = em * gfx::space::sm;
		const float h = pitch + inset * 2;
		float natural = 0;
		bool details = false;
		for (const auto& option : options) {
			natural = std::max(natural, fonts.width(font, option.value("label", stringValue(option.at("value")))));
			details |= option.contains("detail");
		}
		const float most = std::max(1.0f, cache.width - em * gfx::space::lg * 2);
		const float width =
		    std::clamp(std::max(natural + padding * 2 + inset * 2, details ? em * 16 : 0.0f), anchor.w, most);
		auto& popup = cache.popup;
		if (popup.row != id) {
			popup = {.row = id};
			// A popup already present in the first scene must paint its final replay pixels.
			popup.opening.snap(cache.painted ? 0.0f : 1.0f);
			popup.opening.to(1, SDL_GetTicks(), 140);
		}
		popup.anchor = anchor;
		popup.width = width;
		const auto option = stringValue(selected);
		if (popup.selected != option) {
			popup.selected = option;
			popup.revealSelection = true;
		}
		popup.selectedTop = popup.selectedHeight = 0;
		popupMode = true;
		const auto backgroundAt = cache.shapes.size();
		shape({0, 0, width, 0}, card, em * .45f);
		float y = inset;
		for (size_t i = 0; i < options.size(); ++i) {
			const auto& option = options[i];
			const auto optionId = stringValue(option.at("value"));
			const bool on = selected == option.at("value") || selected == json(i);
			SDL_FRect optionRect{inset, y, width - inset * 2, h};
			const auto highlightAt = cache.shapes.size();
			shape(optionRect, on ? text::palette::surface2 : card, em * .3f);
			label(option.value("label", optionId),
			      {inset + padding, y + inset, std::max(1.0f, optionRect.w - padding * 2), pitch},
			      on ? text::palette::accent : text::palette::fg, font, true, 1);
			if (option.contains("detail"))
				optionRect.h +=
				    gap
				    + label(option.at("detail"),
				            {inset + padding, y + inset + pitch + gap, std::max(1.0f, optionRect.w - padding * 2), 0},
				            text::palette::muted)
				    + inset;
			cache.shapes[highlightAt].rect = optionRect;
			if (on) {
				popup.selectedTop = optionRect.y;
				popup.selectedHeight = optionRect.h;
			}
			if (!off)
				target(optionRect, view::TargetKind::Select, id + "=" + optionId,
				       json{{"item", id}, {"value", option.at("value")}});
			y += optionRect.h;
		}
		popup.height = y + inset;
		cache.shapes[backgroundAt].rect.h = popup.height;
		shape({0, 0, width, popup.height}, text::palette::border, em * .45f, true);
		popupMode = false;
		// The body scrolls to keep the pill (not the menu, which places itself) in view.
		cache.optionRect = anchor;
	}
	float control(const json& row, const json& editing, SDL_FRect rect) {
		const auto& control = row.at("control");
		const auto kind = control.value("k", "");
		const auto id = row.value("id", "");
		const bool off = disabled(row), active = editing.is_object() && editing.value("row", "") == id;
		const auto color = off ? text::palette::dim : text::palette::fg;
		const float padding = em * gfx::space::md, inset = em * gfx::space::xs, gap = em * gfx::space::sm;
		const float h = pitch + inset * 2;
		if (kind == "switch") {
			const bool on = control.value("on", false);
			const SDL_FRect track{rect.x + rect.w - em * 2.25f, rect.y + (h - em * 1.3f) * .5f, em * 2.25f, em * 1.3f};
			shape(track, on && !off ? text::palette::accent : text::palette::surface2, track.h * .5f);
			shape(track, text::palette::border, track.h * .5f, true);
			const float side = track.h - unit * 4;
			shape({track.x + (on ? track.w - side - unit * 2 : unit * 2), track.y + unit * 2, side, side},
			      off ? text::palette::dim : text::palette::fg, side * .5f);
			if (!off)
				target({rect.x, rect.y, rect.w, h}, view::TargetKind::Change, id, json(!on));
		} else if (kind == "choice") {
			const auto& options = array(control, "options");
			const auto value = control.value("value", json{});
			const auto mode = control.value("style", "auto");
			const bool segmented = mode == "segmented" || (mode == "auto" && options.size() <= 3);
			const auto font = control.value("mono", false) ? text::FontId::Mono : text::FontId::Ui;
			float controlBottom = rect.y + h;
			if (segmented && !options.empty()) {
				float x = rect.x, y = rect.y;
				for (const auto& option : options) {
					const float width =
					    std::min(rect.w, fonts.width(font, option.value("label", stringValue(option.at("value"))))
					                         + padding * 2);
					if (x > rect.x && x + width > rect.x + rect.w) {
						x = rect.x;
						y += h + gap;
					}
					button(option.value("label", stringValue(option.at("value"))), {x, y, width, h},
					       option.at("value") == value, off, view::TargetKind::Change, id, option.at("value"), font);
					x += width + gap;
				}
				controlBottom = y + h;
			} else {
				std::string current = stringValue(value);
				for (const auto& option : options)
					if (option.at("value") == value)
						current = option.value("label", current);
				const float width = std::min(rect.w, fonts.width(font, current) + padding * 2 + gap + em);
				const SDL_FRect pill{rect.x + rect.w - width, rect.y, width, h};
				shape(pill, text::palette::surface2, h * .5f);
				shape(pill, text::palette::border, h * .5f, true);
				label(current,
				      {pill.x + padding, pill.y + inset, std::max(1.0f, width - padding * 2 - gap - em), pitch}, color,
				      font, true, 1);
				label("⌄", {pill.x + width - padding - em, pill.y + inset, em, pitch}, text::palette::muted,
				      text::FontId::Ui, true, 1);
				if (!off) {
					target(pill, view::TargetKind::Activate, id);
					cache.targets.back().pill = true;
				}
			}
			if (active && !editing.value("option", json{}).is_null())
				menu(options, editing.at("option"), id, font, off, {rect.x, rect.y, rect.w, controlBottom - rect.y});
			return controlBottom - rect.y;
		} else if (kind == "number") {
			const double value =
			    cache.dragItem == id && cache.dragValue ? *cache.dragValue : control.value("value", 0.0);
			std::string display = std::format("{:g}", value);
			if (control.contains("labels")) {
				const auto& labels = control.at("labels");
				if (labels.is_object())
					display = labels.value(display, display);
				else if (labels.is_array() && value >= 0 && static_cast<size_t>(value) < labels.size())
					display = stringValue(labels[static_cast<size_t>(value)]);
			}
			if (!control.value("unit", "").empty())
				display += " " + control.value("unit", "");
			label(display, {rect.x, rect.y + inset, rect.w, pitch}, color, text::FontId::Mono, true, 1);
			const bool slider = control.contains("min") && control.contains("max");
			const SDL_FRect track{rect.x, rect.y + h + gap, rect.w, unit * 4};
			if (slider) {
				const double minimum = control.value("min", 0.0), maximum = control.value("max", 1.0);
				const float ratio = static_cast<float>(
				    maximum == minimum ? 0 : std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0));
				shape(track, text::palette::surface2, unit * 2);
				shape({track.x, track.y, track.w * ratio, track.h}, text::palette::accent, unit * 2);
				shape({track.x + track.w * ratio - unit * 4, track.y - unit * 2, unit * 8, unit * 8}, color, unit * 4);
			}
			const float height = h + (slider ? gap + unit * 6 : 0);
			if (!off)
				target({rect.x, rect.y, rect.w, height}, view::TargetKind::Number, id, control);
			return height;
		} else if (kind == "text") {
			std::string value =
			    active ? editing.value("draft", control.value("value", "")) : control.value("value", "");
			const auto cursor = active ? editing.value("cursor", text::utf16Length(value)) : 0;
			const bool secret = control.value("secret", false);
			if (secret)
				value = std::string(text::utf16Length(value), '*');
			const bool empty = value.empty();
			if (empty)
				value = control.value("placeholder", "");
			shape({rect.x, rect.y, rect.w, h}, text::palette::surface2, em * .35f);
			shape({rect.x, rect.y, rect.w, h}, active ? text::palette::accent : text::palette::border, em * .35f, true);
			label(value, {rect.x + padding, rect.y + inset, std::max(1.0f, rect.w - padding * 2), pitch},
			      empty ? text::palette::dim : color,
			      control.value("mono", false) ? text::FontId::Mono : text::FontId::Ui, true, 1,
			      active ? std::optional<size_t>(text::byteOffset(value, cursor)) : std::nullopt);
			if (!off)
				target({rect.x, rect.y, rect.w, h}, view::TargetKind::Activate, id);
		} else if (kind == "keys") {
			float x = rect.x, y = rect.y;
			for (const auto& combo : array(control, "keys")) {
				float comboWidth = 0;
				for (const auto& key : combo)
					comboWidth +=
					    std::min(rect.w, fonts.width(text::FontId::Ui, keyCap(stringValue(key))) + gap * 2) + gap;
				if (!combo.empty())
					comboWidth -= gap;
				if (x > rect.x && x + comboWidth > rect.x + rect.w) {
					x = rect.x;
					y += h + gap;
				}
				for (const auto& key : combo) {
					const auto caption = keyCap(stringValue(key));
					const float width = std::min(rect.w, fonts.width(text::FontId::Ui, caption) + gap * 2);
					if (x > rect.x && x + width > rect.x + rect.w) {
						x = rect.x;
						y += h + gap;
					}
					shape({x, y, width, h}, text::palette::surface2, unit * 4);
					shape({x, y, width, h}, text::palette::border, unit * 4, true);
					label(caption, {x + gap, y + inset, std::max(1.0f, width - gap * 2), pitch}, color,
					      text::FontId::Ui, true, 1);
					x += width + gap;
				}
				x += padding - gap;
			}
			if (!off)
				target({rect.x, rect.y, rect.w, y - rect.y + h}, view::TargetKind::Activate, id);
			return y - rect.y + h;
		} else if (kind == "multi") {
			const auto& values = array(control, "values");
			float y = rect.y;
			for (const auto& option : array(control, "options")) {
				const auto& value = option.at("value");
				const bool on = std::find(values.begin(), values.end(), value) != values.end();
				const SDL_FRect box{rect.x, y + (h - em) * .5f, em, em};
				shape(box, on ? text::palette::accent : text::palette::surface2, unit * 3);
				shape(box, text::palette::border, unit * 3, true);
				if (on)
					label("✓", {box.x, box.y - unit * 2, box.w, pitch}, sheet, text::FontId::Bold, true, 1);
				label(option.value("label", stringValue(value)),
				      {rect.x + em + gap, y + inset, std::max(1.0f, rect.w - em - gap), pitch}, color, text::FontId::Ui,
				      true, 1);
				json next = json::array();
				for (const auto& previous : values)
					if (previous != value)
						next.push_back(previous);
				if (!on)
					next.push_back(value);
				if (!off)
					target({rect.x, y, rect.w, h}, view::TargetKind::Change, id, std::move(next));
				y += h + gap;
			}
			return y > rect.y ? y - rect.y - gap : h;
		} else if (kind == "action") {
			button(control.value("label", "Run"), {rect.x, rect.y, rect.w, h}, false, off, view::TargetKind::Action,
			       control.value("act", ""), json(id));
		}
		return h;
	}
	PrefsCache& cache;
	text::Engine& engine;
	const text::Fonts& fonts;
	float em, unit, pitch;
	bool popupMode = false;
};

void rebuild(const tsp::Node& node, PrefsCache& cache, text::Engine& engine, const text::Fonts& fonts, float width) {
	cache.texts.clear();
	cache.shapes.clear();
	cache.targets.clear();
	cache.caret.reset();
	cache.previewBox = {};
	cache.focusedRect = {};
	cache.optionRect = {};
	cache.popup.height = 0; // Rebuilt by an open choice; its scroll survives while it stays open.
	cache.width = width;
	Builder b(cache, engine, fonts);
	const float em = b.em, pitch = b.pitch, pad = em * gfx::space::lg;
	const float gap = em * gfx::space::sm, inset = em * gfx::space::md;
	const float inner = std::max(1.0f, width - pad * 2);
	const float badge = em * 1.5f, headingHeight = std::max(badge, pitch);
	const float headingTop = pad + (headingHeight - pitch) * .5f;
	const float titleX = pad + badge + gap, closeX = width - pad - em;
	b.shape({pad, pad, badge, badge}, text::palette::accent, em * .45f, false, false);
	b.label("π", {pad + (badge - em) * .5f, headingTop, em, pitch}, sheet, text::FontId::Bold, false, 1);
	b.label(node.prop("title"), {titleX, headingTop, std::max(1.0f, closeX - gap - titleX), pitch}, text::palette::fg,
	        text::FontId::Bold, false, 1);
	b.label("×", {closeX, headingTop, em, pitch}, text::palette::muted, text::FontId::Ui, false, 1);
	b.target({closeX - gap, pad, em + gap * 2, headingHeight}, view::TargetKind::Action, "close", {}, false);
	float y = pad + headingHeight + pad;
	const auto& query = node.prop("query");
	{
		const float h = pitch + gap * 2;
		b.shape({pad, y, inner, h}, sheet, h * .5f, false, false);
		b.shape({pad, y, inner, h}, text::palette::border, h * .5f, true, false);
		b.label(gfx::iconGlyph("search"), {pad + inset, y + gap, em, pitch}, text::palette::muted, text::FontId::Icons,
		        false, 1);
		const std::string value(node.str("query"));
		const bool queryFocus =
		    query.is_string() && (node.prop("focus").is_null() || !value.empty()) && node.prop("editing").is_null();
		b.label(value.empty() ? "Search settings" : value,
		        {pad + inset + em + gap, y + gap, std::max(1.0f, inner - inset * 2 - em - gap), pitch},
		        value.empty() ? text::palette::dim : text::palette::fg, text::FontId::Ui, false, 1,
		        queryFocus
		            ? std::optional<size_t>(
		                  value.empty()
		                      ? 0
		                      : text::byteOffset(value, static_cast<size_t>(node.num(
		                                                    "cursor", static_cast<double>(text::utf16Length(value))))))
		            : std::nullopt);
		b.target({pad, y, inner, h}, view::TargetKind::Focus, {}, {}, false);
		y += h + gap;
	}
	const auto& pages = node.prop("pages");
	std::string pageLabel;
	size_t pageIndex = 0;
	if (pages.is_array() && !pages.empty()) {
		const float cell = inner / static_cast<float>(pages.size()), h = std::max(em * 2.5f, pitch + gap * 2);
		std::string previousGroup;
		for (size_t i = 0; i < pages.size(); ++i) {
			const auto& page = pages[i];
			const auto id = page.value("id", "");
			const bool active = id == node.str("page"), off = disabled(page);
			const float x = pad + cell * static_cast<float>(i);
			const auto group = page.value("group", "");
			if (i > 0 && group != previousGroup && !group.empty())
				b.shape({x - b.unit, y + (h - em * 1.1f) * .5f, b.unit, em * 1.1f}, text::palette::border, 0, false,
				        false);
			previousGroup = group;
			auto icon = gfx::iconGlyph(page.value("icon", ""));
			const auto iconFont = icon.empty() ? text::FontId::Ui : text::FontId::Icons;
			if (icon.empty())
				icon = page.value("label", id).substr(0, 1);
			b.label(icon, {x + (cell - em) * .5f, y + (h - pitch) * .5f, em * 1.3f, pitch},
			        off      ? text::palette::dim
			        : active ? text::palette::accent
			                 : text::palette::muted,
			        iconFont, false, 1);
			const auto changed = page.value("changed", 0);
			if (changed > 0) {
				const float badge = em * .8f;
				b.shape({x + cell * .6f, y, badge, badge}, text::palette::accent, badge * .5f, false, false);
				b.label(std::to_string(changed), {x + cell * .6f + b.unit, y - b.unit * 2, badge, pitch}, sheet,
				        text::FontId::Ui, false, 1);
			}
			if (active) {
				pageLabel = page.value("label", id);
				pageIndex = i;
				b.shape({x + cell * .2f, y + h - b.unit * 2, cell * .6f, b.unit * 2}, text::palette::accent, b.unit,
				        false, false);
			}
			if (!off)
				b.target({x, y, cell, h}, view::TargetKind::Action, "page", json(id), false);
		}
		y += h;
	}
	cache.headerHeight = y;
	y = pad;
	b.label(std::format("{:02} · {}", pageIndex + 1, pageLabel), {pad, y, inner, pitch}, text::palette::accent,
	        text::FontId::Mono, true, 1);
	y += pitch + gap;
	y += b.label(pageLabel, {pad, y, inner, pitch * 2}, text::palette::fg, text::FontId::Wordmark, true, 1) + gap;
	if (!node.prop("lead").is_null())
		y += b.label(node.prop("lead"), {pad, y, inner, 0}, text::palette::muted) + pad;
	b.shape({pad, y, inner, b.unit}, text::palette::border);
	y += pad;
	const auto& sections = node.prop("sections");
	if (sections.is_array())
		for (const auto& section : sections) {
			if (section.contains("page") && section.at("page") != node.prop("page") && node.str("query").empty())
				continue;
			if (array(section, "rows").empty())
				continue;
			const float sectionTop = y;
			const float sectionHeight = b.label(section.value("title", ""), {pad, y, inner, pitch},
			                                    text::palette::muted, text::FontId::Ui, true, 1);
			y += sectionHeight + gap;
			if (node.str("focus") == section.value("id", "")) {
				cache.focusedRect = {pad, sectionTop, inner, sectionHeight};
				b.shape(cache.focusedRect, text::palette::accent, em * .3f, true);
			}
			bool statusSection = false;
			const float rowsTop = y;
			for (const auto& row : array(section, "rows")) {
				const auto id = row.value("id", "");
				statusSection |= id.starts_with("statusLine.");
				const bool off = disabled(row), focus = node.str("focus") == id;
				const float top = y, rowInset = em * gfx::space::lg;
				const float contentWidth = std::max(1.0f, inner - rowInset * 2);
				const bool hasControl = row.contains("control");
				const float controlWidth = hasControl ? b.controlWidth(row.at("control"), contentWidth) : 0;
				const float labelWidth = std::max(1.0f, contentWidth - controlWidth - (hasControl ? inset : 0));
				const float x = pad + rowInset;
				const float textTop = y + rowInset;
				float bottom = textTop;
				const auto shapeAt = cache.shapes.size();
				b.shape({pad, y, inner, pitch}, card, em * .6f);
				b.target({pad, y, inner, pitch}, off ? view::TargetKind::Block : view::TargetKind::Select, id);
				const auto hitAt = cache.targets.size() - 1;
				bottom += b.label(row.value("label", id), {x, bottom, labelWidth, 0},
				                  off ? text::palette::dim : text::palette::fg, text::FontId::Bold);
				if (row.contains("hint")) {
					bottom += gap;
					bottom += b.label(row.at("hint"), {x, bottom, labelWidth, 0}, text::palette::muted);
				}
				if (row.contains("warning")) {
					bottom += gap;
					bottom += b.label(row.at("warning"), {x, bottom, labelWidth, 0}, text::palette::warning);
				}
				if (row.value("changed", false)) {
					b.shape({pad + (rowInset - b.unit * 4) * .5f, textTop + pitch * .45f, b.unit * 4, b.unit * 4},
					        text::palette::accent, b.unit * 2);
					if (row.contains("defaultLabel")) {
						bottom += gap;
						const float defaultTop = bottom;
						bottom += b.label("Default: " + text::plainText(row.at("defaultLabel")),
						                  {x, bottom, labelWidth, 0}, text::palette::dim);
						if (!off)
							b.target({x, defaultTop, labelWidth, bottom - defaultTop}, view::TargetKind::Change, id,
							         json(nullptr));
					}
				}
				if (hasControl)
					bottom = std::max(
					    bottom, textTop
					                + b.control(row, node.prop("editing"),
					                            {pad + inner - controlWidth - rowInset, textTop, controlWidth, pitch}));
				y = bottom + rowInset;
				cache.shapes[shapeAt].rect.h = y - top;
				cache.targets[hitAt].rect.h = y - top;
				if (focus) {
					cache.focusedRect = {pad, top, inner, y - top};
				}
				if (off && row.at("disabled").is_string())
					b.target({pad, top, inner, y - top}, view::TargetKind::Hover,
					         row.at("disabled").get<std::string>());
				b.shape({pad + rowInset, y - b.unit, contentWidth, b.unit}, text::palette::border);
			}
			b.shape({pad, rowsTop, inner, y - rowsTop}, text::palette::border, em * .6f, true);
			if (cache.focusedRect.y >= rowsTop && cache.focusedRect.y < y)
				b.shape(cache.focusedRect, text::palette::accent, em * .6f, true);
			if (statusSection && cache.previewHeight > 0) {
				cache.previewBox = {pad, y + gap, inner, cache.previewHeight};
				y += cache.previewHeight + gap * 2;
			}
			y += pad;
		}
	cache.bodyHeight = y;
	if (cache.popup.height == 0)
		cache.popup = {};
}
} // namespace

void syncPrefs(const tsp::Node& node, PrefsCache& cache, text::Engine&) {
	cache.width = -1;
	if (cache.page != node.str("page")) {
		cache.page = node.str("page");
		cache.scroll = 0;
		cache.scrollTween.snap(0);
	}
	const bool focusChanged = cache.shownFocus != node.str("focus");
	if (focusChanged) {
		cache.shownFocus = node.str("focus");
		cache.revealFocus = !cache.shownFocus.empty();
	}
	const auto& editing = node.prop("editing");
	const auto option = editing.is_object() ? stringValue(editing.value("option", json{})) : std::string{};
	const bool optionChanged = option != cache.shownOption;
	if (optionChanged) {
		cache.shownOption = option;
		cache.revealFocus |= !option.empty();
		cache.popup.revealSelection = true;
	}
	if (focusChanged || optionChanged) {
		cache.scroll = cache.scrollTween.value(SDL_GetTicks());
		cache.scrollTween.snap(cache.scroll);
	}
}
SDL_FPoint measurePrefs(const tsp::Node& node, PrefsCache& cache, text::Engine& engine, const text::Fonts& fonts,
                        float width) {
	if (cache.width != width)
		rebuild(node, cache, engine, fonts, width);
	return {width, cache.headerHeight + cache.bodyHeight};
}
std::optional<SDL_FRect> paintPrefs(const tsp::Node& node, PrefsCache& cache, text::Engine& engine,
                                    const text::Fonts& fonts, SDL_Renderer& renderer, const SDL_FRect& rect,
                                    bool focused, view::HitTargets& hits) {
	measurePrefs(node, cache, engine, fonts, rect.w);
	const auto now = SDL_GetTicks();
	const float unit = fonts.size() / 15;
	gfx::fill(renderer, rect, sheet);
	gfx::line(renderer, {rect.x, rect.y}, {rect.x, rect.y + rect.h}, text::palette::border, unit);
	cache.bodyRect = {rect.x, rect.y + cache.headerHeight, rect.w, std::max(0.0f, rect.h - cache.headerHeight)};
	cache.viewportHeight = cache.bodyRect.h;
	cache.scrollMax = std::max(0.0f, cache.bodyHeight - cache.viewportHeight);
	const float destination = std::clamp(cache.scrollTween.target(), 0.0f, cache.scrollMax);
	if (destination != cache.scrollTween.target())
		cache.scrollTween.snap(destination);
	cache.scroll = std::clamp(cache.scrollTween.value(now), 0.0f, cache.scrollMax);
	if (cache.revealFocus) {
		const auto& focus = cache.optionRect.h > 0 ? cache.optionRect : cache.focusedRect;
		if (focus.y < cache.scroll)
			cache.scroll = focus.y;
		else if (focus.y + focus.h > cache.scroll + cache.viewportHeight)
			cache.scroll = focus.y + focus.h - cache.viewportHeight;
		cache.scroll = std::clamp(cache.scroll, 0.0f, cache.scrollMax);
		cache.scrollTween.snap(cache.scroll);
		cache.revealFocus = false;
	}
	cache.transitioning = cache.scrollTween.active(now);
	gfx::ScopedClip outerClip(renderer, rect);
	hits.add(rect, view::TargetKind::Focus);
	for (const bool body : {false, true}) {
		const auto clip = body ? cache.bodyRect : SDL_FRect{rect.x, rect.y, rect.w, cache.headerHeight};
		gfx::ScopedClip bodyClip(renderer, clip);
		const float y = body ? cache.bodyRect.y - cache.scroll : rect.y;
		for (const auto& shape : cache.shapes)
			if (shape.body == body && !shape.popup) {
				const auto box = moved(shape.rect, rect.x, y);
				if (!gfx::intersects(box, clip))
					continue;
				if (shape.border)
					gfx::roundedBorder(renderer, box, shape.radius, shape.color, unit);
				else
					gfx::rounded(renderer, box, shape.radius, shape.color);
			}
		for (const auto& item : cache.texts)
			if (item.body == body && !item.popup) {
				const auto box = moved({item.rect.x, item.rect.y, item.rect.w, item.layout.height}, rect.x, y);
				if (gfx::intersects(box, clip))
					engine.draw(item.runs, item.layout, box.x, box.y);
			}
		for (const auto& target : cache.targets)
			if (target.body == body && !target.popup) {
				const auto box = gfx::intersect(moved(target.rect, rect.x, y), clip);
				hits.add(box, target.kind, target.item, 0, target.value);
			}
	}
	std::optional<SDL_FRect> caret;
	if (focused && cache.caret) {
		caret = moved(*cache.caret, rect.x, cache.caretBody ? cache.bodyRect.y - cache.scroll : rect.y);
		if (gfx::intersects(*caret, cache.caretBody ? cache.bodyRect : rect))
			gfx::fill(renderer, *caret, text::palette::accent);
	}
	if (cache.scrollMax > 0) {
		const float h = std::max(fonts.size(), cache.viewportHeight * cache.viewportHeight / cache.bodyHeight);
		gfx::rounded(renderer,
		             {rect.x + rect.w - unit * 4,
		              cache.bodyRect.y + (cache.viewportHeight - h) * cache.scroll / cache.scrollMax, unit * 2, h},
		             unit, text::palette::dim);
	}
	auto& popup = cache.popup;
	popup.rect = {};
	if (popup.height > 0) {
		// Below the pill when it fits (or has the most room), else above; within the body.
		const auto anchor = moved(popup.anchor, rect.x, cache.bodyRect.y - cache.scroll);
		const float gap = unit * 4, top = cache.bodyRect.y + gap, bottom = cache.bodyRect.y + cache.bodyRect.h - gap;
		const float below = bottom - (anchor.y + anchor.h + gap), above = anchor.y - gap - top;
		const bool down = below >= popup.height || below >= above;
		const float h = std::max(0.0f, std::min(popup.height, down ? below : above));
		const float x = std::clamp(anchor.x + anchor.w - popup.width, rect.x + gap,
		                           std::max(rect.x + gap, rect.x + rect.w - popup.width - gap));
		SDL_FRect frame{x, down ? anchor.y + anchor.h + gap : anchor.y - gap - h, popup.width, h};
		// Selection and viewport changes reveal immediately; wheel destinations survive ordinary rebuilds.
		if (popup.revealSelection || popup.viewportHeight != h) {
			popup.scroll = popup.scrollTween.value(now);
			const float least = std::min(popup.selectedTop, popup.selectedTop + popup.selectedHeight - h);
			popup.scroll = std::clamp(popup.scroll, least, popup.selectedTop);
			popup.scroll = std::clamp(popup.scroll, 0.0f, std::max(0.0f, popup.height - h));
			popup.scrollTween.snap(popup.scroll);
			popup.revealSelection = false;
		}
		popup.viewportHeight = h;
		const float menuDestination = std::clamp(popup.scrollTween.target(), 0.0f, std::max(0.0f, popup.height - h));
		if (menuDestination != popup.scrollTween.target())
			popup.scrollTween.snap(menuDestination);
		popup.scroll = popup.scrollTween.value(now);
		const float opening = popup.opening.value(now), visibleHeight = h * opening;
		const SDL_FRect reveal{x, down ? frame.y : frame.y + h - visibleHeight, popup.width, visibleHeight};
		frame.y += (down ? -1 : 1) * unit * 6 * (1 - opening);
		popup.rect = gfx::intersect(gfx::intersect(frame, reveal), cache.bodyRect);
		cache.transitioning |= popup.scrollTween.active(now) || popup.opening.active(now);
		// A click anywhere else on the sheet closes the menu, but another pill opens its own.
		hits.add(rect, view::TargetKind::Action, "close", 0, json(popup.row));
		for (const auto& target : cache.targets)
			if (target.pill)
				hits.add(gfx::intersect(moved(target.rect, rect.x, cache.bodyRect.y - cache.scroll), cache.bodyRect),
				         target.kind, target.item, 0, target.value);
		gfx::ScopedClip popupBodyClip(renderer, cache.bodyRect);
		if (popup.rect.h > 0)
			for (int i = 3; i >= 1; --i) {
				const float spread = unit * 3 * static_cast<float>(i);
				gfx::rounded(
				    renderer,
				    {x - spread, popup.rect.y - spread + unit * 2, popup.width + spread * 2, popup.rect.h + spread * 2},
				    fonts.size() * .45f + spread, text::rgb(0, static_cast<Uint8>(22 / i)));
			}
		gfx::ScopedClip menuClip(renderer, popup.rect);
		const float y = frame.y - popup.scroll;
		for (const auto& shape : cache.shapes)
			if (shape.popup) {
				auto box = moved(shape.rect, x, y);
				// The frame spans the visible part, so its rounded corners stay in view.
				if (shape.rect.y == 0 && shape.rect.h == popup.height)
					box = frame;
				if (shape.border)
					gfx::roundedBorder(renderer, box, shape.radius, shape.color, unit);
				else
					gfx::rounded(renderer, box, shape.radius, shape.color);
			}
		for (const auto& item : cache.texts)
			if (item.popup) {
				const auto box = moved({item.rect.x, item.rect.y, item.rect.w, item.layout.height}, x, y);
				if (gfx::intersects(box, popup.rect))
					engine.draw(item.runs, item.layout, box.x, box.y);
			}
		for (const auto& target : cache.targets)
			if (target.popup)
				hits.add(gfx::intersect(moved(target.rect, x, y), popup.rect), target.kind, target.item, 0,
				         target.value);
	}
	cache.painted = true;
	return caret;
}
bool scrollPrefs(PrefsCache& cache, float x, float y, float delta) {
	if (contains(cache.popup.rect, x, y)) {
		auto& popup = cache.popup;
		popup.scrollTween.to(
		    std::clamp(popup.scrollTween.target() + delta, 0.0f, std::max(0.0f, popup.height - popup.viewportHeight)),
		    SDL_GetTicks());
		return true;
	}
	if (!contains(cache.bodyRect, x, y))
		return false;
	cache.scrollTween.to(std::clamp(cache.scrollTween.target() + delta, 0.0f, cache.scrollMax), SDL_GetTicks());
	return true;
}
double dragPrefsNumber(PrefsCache& cache, std::string_view item, const json& control, const SDL_FRect& track,
                       float startX, float x, float scale) {
	cache.scroll = cache.scrollTween.value(SDL_GetTicks());
	cache.scrollTween.snap(cache.scroll);
	const double step = control.value("step", 1.0);
	const bool slider = control.contains("min") && control.contains("max");
	const double minimum = control.value("min", 0.0);
	const double maximum = control.value("max", 1.0);
	double value =
	    slider ? minimum + std::clamp(static_cast<double>((x - track.x) / track.w), 0.0, 1.0) * (maximum - minimum)
	           : control.value("value", 0.0) + static_cast<double>((x - startX) / (4 * scale)) * step;
	if (step > 0)
		value = minimum + std::round((value - minimum) / step) * step;
	if (control.contains("min"))
		value = std::max(value, minimum);
	if (control.contains("max"))
		value = std::min(value, maximum);
	cache.dragItem = item;
	cache.dragValue = value;
	cache.width = -1;
	return value;
}

} // namespace kinds
