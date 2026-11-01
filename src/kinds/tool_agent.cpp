#include "tool_agent.h"

#include "code.h"
#include "gfx/draw.h"
#include "gfx/icons.h"
#include "view/hit_targets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <numbers>
#include <utility>

namespace kinds {
namespace {

using Row = ToolAgentCache::Row;
constexpr float naturalWidth = 1000000;
constexpr float pi = std::numbers::pi_v<float>;

text::RunStyle style(SDL_Color color, text::FontId font = text::FontId::Ui) {
	text::RunStyle result;
	result.color = color;
	result.font = font;
	return result;
}

SDL_Color tint(SDL_Color color, Uint8 alpha) {
	color.a = alpha;
	return color;
}

SDL_Color tone(std::string_view token, SDL_Color fallback) {
	auto result = style(fallback);
	text::applyTokens(token, result);
	return result.color;
}

void append(text::Runs& into, text::Runs runs) {
	into.insert(into.end(), std::make_move_iterator(runs.begin()), std::make_move_iterator(runs.end()));
}

void add(text::Runs& runs, std::string_view value, const text::RunStyle& base) {
	if (!value.empty())
		runs.push_back({std::string(value), base});
}

std::string duration(double milliseconds) {
	const auto seconds = static_cast<int64_t>(std::max(0.0, milliseconds) / 1000);
	if (seconds < 60)
		return std::to_string(seconds) + "s";
	if (seconds < 3600)
		return std::to_string(seconds / 60) + "m " + std::to_string(seconds % 60) + "s";
	return std::to_string(seconds / 3600) + "h " + std::to_string(seconds / 60 % 60) + "m";
}

std::string count(double value) {
	char buffer[32];
	if (value >= 1000000)
		std::snprintf(buffer, sizeof buffer, "%.1fM", value / 1000000);
	else if (value >= 1000)
		std::snprintf(buffer, sizeof buffer, "%.1fK", value / 1000);
	else
		std::snprintf(buffer, sizeof buffer, "%.0f", value);
	return buffer;
}

std::pair<std::string_view, SDL_Color> statusMark(std::string_view status) {
	if (status == "done")
		return {"✓", text::palette::success};
	if (status == "error" || status == "failed")
		return {"✗", text::palette::error};
	if (status == "cancelled" || status == "aborted")
		return {"⊘", text::palette::muted};
	if (status == "running")
		return {{}, text::palette::accent};
	if (status == "parked")
		return {"Ⅱ", text::palette::pending};
	if (status == "idle")
		return {"○", text::palette::dim};
	return {"○", text::palette::muted};
}

std::pair<std::string_view, SDL_Color> itemMark(std::string_view status) {
	if (status == "done")
		return {"✓", text::palette::success};
	if (status == "dropped")
		return {"✗", text::palette::dim};
	if (status == "active") // ● rather than ◐: the UI face has no half circle and a fallback sits off the baseline.
		return {"●", text::palette::accent};
	if (status == "blocked")
		return {"⚠", text::palette::warning};
	return {"○", text::palette::muted};
}

template <class Function>
void eachRow(ToolAgentCache& cache, Function&& function) {
	function(cache.mark);
	function(cache.head);
	function(cache.after);
	function(cache.right);
	function(cache.clock);
	function(cache.intent);
	function(cache.currentTool);
	function(cache.currentToolAge);
	function(cache.retry);
	function(cache.retryClock);
	function(cache.hud);
	for (auto& chip : cache.chips)
		function(chip.label);
	for (auto& button : cache.buttons)
		function(button.label);
	for (auto& phase : cache.phases) {
		function(phase.title);
		for (auto& item : phase.items) {
			function(item.mark);
			function(item.label);
			function(item.note);
		}
	}
}

void prepare(Row& row, text::Engine& engine) {
	row.layout = engine.layout(row.runs, {.width = naturalWidth, .wrap = text::Wrap::None, .maxLines = 1});
	row.naturalWidth = row.layout.width;
	row.rect = {0, 0, -1, 0};
	row.effects = std::any_of(row.runs.begin(), row.runs.end(), [](const text::Run& run) { return run.style.fx != 0; });
}

void place(Row& row, text::Engine& engine, float x, float y, float width, bool wrap = false) {
	width = std::max(0.0f, width);
	if (row.rect.w != width)
		row.layout = engine.layout(
		    row.runs, {.width = width, .wrap = wrap ? text::Wrap::Word : text::Wrap::None, .maxLines = wrap ? 0 : 1});
	row.rect = {x, y, width, row.layout.height};
}

void replaceTimer(Row& row, std::string value, text::Engine& engine) {
	engine.forget(row.runs);
	row.runs.clear();
	add(row.runs, value, style(text::palette::dim, text::FontId::Mono));
	prepare(row, engine);
}

bool updateClocks(const tsp::Node& node, ToolAgentCache& cache, text::Engine& engine) {
	bool changed = false;
	if (cache.clocked || cache.hasTook) {
		const double elapsed =
		    cache.clocked ? cache.age + node.since(node.kind == tsp::Kind::Tool ? "age" : "stats") : cache.took;
		const auto seconds = static_cast<int64_t>(std::max(0.0, elapsed) / 1000);
		const auto second = seconds < 3600 ? seconds : seconds / 60 * 60;
		if (second != cache.clockSecond) {
			replaceTimer(cache.clock, duration(elapsed), engine);
			cache.clockSecond = second;
			changed = true;
		}
	}
	if (cache.toolClocked) {
		const double elapsed = cache.toolAge + node.since("tool");
		const auto seconds = static_cast<int64_t>(std::max(0.0, elapsed) / 1000);
		const auto second = seconds < 3600 ? seconds : seconds / 60 * 60;
		if (second != cache.toolSecond) {
			replaceTimer(cache.currentToolAge, duration(elapsed), engine);
			cache.toolSecond = second;
			changed = true;
		}
	}
	if (cache.retryClocked) {
		const double remaining = std::max(0.0, cache.retryDelay - cache.retryAge - node.since("retry"));
		const auto second = static_cast<int64_t>(std::ceil(remaining / 1000));
		if (second != cache.retrySecond) {
			replaceTimer(cache.retryClock, "in " + std::to_string(second) + "s", engine);
			cache.retrySecond = second;
			changed = true;
		}
	}
	return changed;
}

void badges(const json& values, ToolAgentCache& cache) {
	if (!values.is_array())
		return;
	for (const auto& value : values) {
		auto& chip = cache.chips.emplace_back();
		chip.color = tone(value.value("tone", ""), text::palette::pending);
		add(chip.label.runs, value.value("text", ""), style(chip.color));
		chip.tip = value.value("title", "");
	}
}

void syncTool(const tsp::Node& node, ToolAgentCache& cache) {
	cache.inlineFrame = node.str("frame", "card") == "inline";
	cache.head.runs = text::toRuns(node.prop("title"), style(text::palette::fg, text::FontId::Bold));
	if (cache.head.runs.empty())
		add(cache.head.runs, node.str("name"), style(text::palette::fg, text::FontId::Bold));
	const auto& target = node.prop("target");
	const auto kind = node.str("targetKind", "text");
	if (!target.is_null()) {
		add(cache.head.runs, "  ", style(text::palette::fg));
		if (kind == "path" && target.is_string()) {
			const auto& value = target.get_ref<const std::string&>();
			const auto slash = value.find_last_of("/\\");
			auto directory = style(text::palette::muted, text::FontId::Mono);
			auto filename = style(text::palette::info, text::FontId::MonoBold);
			directory.href = filename.href = node.str("href");
			if (slash != std::string::npos)
				add(cache.head.runs, std::string_view(value).substr(0, slash + 1), directory);
			add(cache.head.runs, std::string_view(value).substr(slash == std::string::npos ? 0 : slash + 1), filename);
		} else if (kind == "command" && target.is_string()) {
			append(cache.head.runs, highlightLine(target.get_ref<const std::string&>(), node.str("lang", "bash")));
		} else {
			// Patterns and queries are machine strings; a text target is prose (an intent), so UI font, muted.
			const bool literal = kind == "pattern" || kind == "query";
			auto base = literal ? style(text::palette::warning, text::FontId::Mono) : style(text::palette::muted);
			if (literal)
				add(cache.head.runs, "\"", base);
			auto runs = text::toRuns(target, base);
			if (!node.str("href").empty())
				for (auto& run : runs)
					run.style.href = node.str("href");
			append(cache.head.runs, std::move(runs));
			if (literal)
				add(cache.head.runs, "\"", base);
		}
	}
	if (node.prop("meta").is_array())
		for (const auto& meta : node.prop("meta")) {
			// The same ` · ` omp uses inside a meta entry ("3 matches · 2 files").
			add(cache.head.runs, " · ", style(text::palette::dim));
			append(cache.head.runs, text::toRuns(meta, style(text::palette::muted)));
		}
	badges(node.prop("badges"), cache);
	if (node.prop("exit").is_number() && node.num("exit") != 0) {
		auto& chip = cache.chips.emplace_back();
		chip.color = text::palette::error;
		add(chip.label.runs, "exit " + std::to_string(static_cast<int>(node.num("exit"))),
		    style(chip.color, text::FontId::Bold));
	}
	cache.after.runs = text::toRuns(node.prop("note"), style(text::palette::warning));
	cache.command = node.prop("command").is_string() ? std::string(node.str("command"))
	              : kind == "command"                ? text::plainText(target)
	                                                 : std::string{};
	if (node.prop("tools").is_array())
		for (const auto& value : node.prop("tools")) {
			auto& button = cache.buttons.emplace_back();
			button.id = value.value("id", "");
			button.primary = value.value("primary", false);
			button.danger = value.value("danger", false);
			button.on = value.value("on", false);
			const auto disabled = value.find("disabled");
			button.disabled =
			    disabled != value.end() && (disabled->is_string() || (disabled->is_boolean() && disabled->get<bool>()));
			if (disabled != value.end() && disabled->is_string())
				button.tip = disabled->get<std::string>();
			add(button.label.runs, value.value("label", button.id),
			    style(button.disabled  ? text::palette::dim
			          : button.danger  ? text::palette::error
			          : button.primary ? text::palette::accent
			                           : text::palette::fg));
			if (value.contains("keys") && value["keys"].is_array()) {
				for (const auto& key : value["keys"])
					add(button.label.runs, " " + key.get<std::string>(), style(text::palette::dim));
			}
		}
	if (node.prop("command").is_string() || kind == "command") {
		auto& button = cache.buttons.emplace_back();
		button.copy = true;
		add(button.label.runs, "Copy", style(text::palette::fg));
	}
	if (cache.running) {
		auto base = style(text::palette::muted, text::FontId::Italic);
		base.fx = 1;
		add(cache.intent.runs, node.str("intent"), base);
	} else
		cache.completedIntent = node.str("intent");
	cache.clocked = cache.running && node.prop("age").is_number();
	cache.hasTook = !cache.clocked && node.prop("took").is_number();
	cache.age = node.num("age");
	cache.took = node.num("took");
}

void syncAgent(const tsp::Node& node, ToolAgentCache& cache) {
	cache.indent = static_cast<float>(std::max(0.0, node.num("depth")));
	add(cache.head.runs, node.str("name"), style(text::palette::fg, text::FontId::Bold));
	if (!node.str("agent").empty()) {
		auto& chip = cache.chips.emplace_back();
		add(chip.label.runs, node.str("agent"), style(text::palette::pending));
	}
	badges(node.prop("badges"), cache);
	cache.after.runs = text::toRuns(node.prop("task"), style(text::palette::muted));
	if (!node.str("model").empty()) {
		if (!node.str("thinking").empty()) {
			auto dot = style(text::palette::pending);
			text::applyTokens(node.str("thinking"), dot);
			add(cache.right.runs, "● ", dot);
		}
		add(cache.right.runs, node.str("model"), style(text::palette::dim));
	}
	const auto& stats = node.prop("stats");
	if (stats.is_object()) {
		auto fact = [&](std::string value) {
			if (!cache.right.runs.empty())
				add(cache.right.runs, " · ", style(text::palette::dim));
			add(cache.right.runs, value, style(text::palette::dim));
		};
		if (stats.contains("tokens"))
			fact(count(stats.value("tokens", 0.0)) + " tok");
		if (stats.contains("cost")) {
			char buffer[48];
			std::snprintf(buffer, sizeof buffer, "$%.2f", stats.value("cost", 0.0));
			fact(buffer);
		}
		cache.clocked = cache.running && stats.contains("age");
		cache.hasTook = !cache.clocked && stats.contains("took");
		cache.age = stats.value("age", 0.0);
		cache.took = stats.value("took", 0.0);
		if (cache.running && stats.contains("done"))
			cache.progress = std::clamp(stats.value("done", 0.0f), 0.0f, 1.0f);
	}
	const auto& tool = node.prop("tool");
	if (tool.is_object()) {
		auto icon = style(text::palette::accent, text::FontId::Icons);
		icon.icon = true;
		add(cache.currentTool.runs, gfx::iconGlyph(tool.value("name", "")), icon);
		add(cache.currentTool.runs, tool.value("name", ""), style(text::palette::accent, text::FontId::Mono));
		if (tool.contains("intent")) {
			add(cache.currentTool.runs, "  ", style(text::palette::fg));
			append(cache.currentTool.runs, text::toRuns(tool["intent"], style(text::palette::muted)));
		}
		cache.toolClocked = tool.contains("age");
		cache.toolAge = tool.value("age", 0.0);
	}
	const auto& retry = node.prop("retry");
	if (retry.is_object()) {
		std::string value =
		    "retry " + std::to_string(retry.value("attempt", 0)) + "/" + std::to_string(retry.value("max", 0));
		if (retry.contains("error"))
			value += ": " + retry.value("error", "");
		add(cache.retry.runs, value, style(text::palette::warning));
		cache.retryClocked = true;
		cache.retryAge = retry.value("age", 0.0);
		cache.retryDelay = retry.value("delay", 0.0);
	}
}

void syncChecklist(const tsp::Node& node, ToolAgentCache& cache, std::vector<ToolAgentCache::Phase>& previous) {
	cache.mode = node.str("mode", "full");
	cache.after.runs = text::toRuns(node.prop("note"), style(text::palette::warning));
	int done = 0, total = 0;
	if (node.prop("phases").is_array())
		for (const auto& value : node.prop("phases")) {
			auto& phase = cache.phases.emplace_back();
			phase.id = value.value("id", "");
			phase.collapsed = value.value("collapsed", false);
			for (const auto& old : previous)
				if (old.id == phase.id && old.collapsed == phase.collapsed) {
					phase.localCollapsed = old.localCollapsed;
					break;
				}
			phase.title.runs = text::toRuns(value.at("title"), style(text::palette::fg, text::FontId::Bold));
			int phaseDone = 0;
			for (const auto& entry : value.at("items")) {
				auto& item = phase.items.emplace_back();
				const auto status = entry.value("status", "pending");
				item.closed = status == "done" || status == "dropped";
				++total;
				if (item.closed) {
					++done;
					++phaseDone;
				}
				const auto [glyph, color] = itemMark(status);
				add(item.mark.runs, glyph, style(color));
				auto base = style(item.closed ? text::palette::muted : text::palette::fg);
				base.strike = status == "dropped";
				item.label.runs = text::toRuns(entry.at("text"), base);
				if (entry.contains("note"))
					item.note.runs = text::toRuns(
					    entry["note"], style(status == "blocked" ? text::palette::warning : text::palette::dim));
			}
			add(phase.title.runs, "  " + std::to_string(phaseDone) + "/" + std::to_string(phase.items.size()),
			    style(text::palette::dim));
		}
	auto icon = style(text::palette::accent, text::FontId::Icons);
	icon.icon = true;
	add(cache.hud.runs, gfx::iconGlyph("todo"), icon);
	add(cache.hud.runs, std::to_string(done) + "/" + std::to_string(total), style(text::palette::accent));
}

SDL_FRect translated(SDL_FRect rect, const SDL_FRect& origin) {
	rect.x += origin.x;
	rect.y += origin.y;
	return rect;
}

bool drawRow(const Row& row, const SDL_FRect& origin, text::Engine& engine, view::HitTargets& targets,
             const SDL_FRect& clip) {
	if (row.runs.empty() || row.rect.w <= 0)
		return false;
	const auto rect = translated(row.rect, origin);
	if (!gfx::intersects(rect, clip))
		return false;
	engine.draw(row.runs, row.layout, rect.x, rect.y);
	for (const auto& piece : row.layout.pieces) {
		const auto& href = row.runs[piece.run].style.href;
		if (href.empty() || piece.ellipsis)
			continue;
		const SDL_FRect link{rect.x + piece.x, rect.y + static_cast<float>(piece.line) * row.layout.lineHeight,
		                     piece.width, row.layout.lineHeight};
		targets.add(gfx::intersect(gfx::intersect(link, rect), clip), view::TargetKind::Link, href);
	}
	return row.effects;
}

void chevron(SDL_Renderer& renderer, SDL_FPoint center, float size, bool collapsed) {
	const float radius = size * 0.22f, thickness = std::max(1.0f, size / 12);
	if (collapsed) {
		gfx::line(renderer, {center.x - radius * 0.5f, center.y - radius}, {center.x + radius * 0.5f, center.y},
		          text::palette::muted, thickness);
		gfx::line(renderer, {center.x + radius * 0.5f, center.y}, {center.x - radius * 0.5f, center.y + radius},
		          text::palette::muted, thickness);
	} else {
		gfx::line(renderer, {center.x - radius, center.y - radius * 0.5f}, {center.x, center.y + radius * 0.5f},
		          text::palette::muted, thickness);
		gfx::line(renderer, {center.x, center.y + radius * 0.5f}, {center.x + radius, center.y - radius * 0.5f},
		          text::palette::muted, thickness);
	}
}

void layoutHeader(ToolAgentCache& cache, text::Engine& engine, const text::Fonts& fonts, float left, float top,
                  float width) {
	const float size = fonts.size(), gap = size * gfx::space::sm;
	const float padX = size * gfx::space::md, padY = size * gfx::space::xs;
	float line = std::max({fonts.lineHeight(), cache.mark.layout.height, cache.head.layout.height,
	                       cache.after.layout.height, cache.right.layout.height, cache.clock.layout.height});
	for (const auto& chip : cache.chips)
		line = std::max(line, chip.label.layout.height);
	for (const auto& button : cache.buttons)
		line = std::max(line, button.label.layout.height);
	cache.headRect = {left, top, width, line + padY * 2};
	const float textTop = top + padY, markWidth = size + gap;
	place(cache.mark, engine, left, textTop, markWidth);
	float right = left + width - (cache.collapsible ? size + gap : 0);
	if (!cache.clock.runs.empty()) {
		const float clockWidth = std::min(cache.clock.naturalWidth, std::max(0.0f, right - left));
		right -= clockWidth;
		place(cache.clock, engine, right, textTop, clockWidth);
		right -= gap;
	}
	if (!cache.right.runs.empty()) {
		const float rightWidth = std::min(cache.right.naturalWidth, std::max(0.0f, (right - left) * 0.55f));
		right -= rightWidth;
		place(cache.right, engine, right, textTop, rightWidth);
		right -= gap;
	}
	const float begin = left + markWidth;
	const float available = std::max(0.0f, right - begin);
	cache.headContentRect = {begin, top, available, cache.headRect.h};
	float adornment = cache.after.runs.empty() ? 0 : cache.after.naturalWidth + gap;
	for (const auto& chip : cache.chips)
		adornment += chip.label.naturalWidth + padX * 2 + gap;
	const float headWidth = std::min(cache.head.naturalWidth, std::max(available * 0.35f, available - adornment));
	place(cache.head, engine, begin, textTop, headWidth);
	float x = begin + cache.head.layout.width;
	for (auto& chip : cache.chips) {
		x += gap + padX;
		place(chip.label, engine, x, textTop, chip.label.naturalWidth);
		x += chip.label.naturalWidth + padX;
	}
	if (!cache.after.runs.empty())
		place(cache.after, engine, x + gap, textTop, std::max(0.0f, right - x - gap));
	float buttonRight = right;
	for (auto& button : cache.buttons) {
		const float width = button.label.naturalWidth + padX * 2;
		buttonRight -= width + gap;
		place(button.label, engine, buttonRight + padX, textTop, button.label.naturalWidth);
	}
}

void layoutChecklist(ToolAgentCache& cache, text::Engine& engine, const text::Fonts& fonts, float width) {
	const float size = fonts.size(), line = fonts.lineHeight();
	const float gap = size * gfx::space::sm, itemGap = size * gfx::space::xs;
	if (cache.mode == "hud") {
		const float padX = size * gfx::space::md, padY = size * gfx::space::xs;
		place(cache.hud, engine, padX, padY, std::max(0.0f, std::min(cache.hud.naturalWidth, width - 2 * padX)));
		cache.ownHeight = cache.hud.rect.h + padY * 2;
		cache.measuredWidth = std::min(width, cache.hud.naturalWidth + 2 * padX);
		cache.headRect = {0, 0, cache.measuredWidth, cache.ownHeight};
		return;
	}
	const bool reminder = cache.mode == "reminder";
	const float pad = reminder ? size * gfx::space::lg : 0;
	const float titleInset = size + gap, markInset = gap, labelInset = markInset + size + gap;
	float y = pad, trailing = 0; // `trailing`: spacing after the last row, not kept at the bottom.
	if (!cache.after.runs.empty()) {
		place(cache.after, engine, pad, y, width - 2 * pad, true);
		y += cache.after.rect.h + gap;
		trailing = gap;
	}
	for (auto& phase : cache.phases) {
		if (!reminder) {
			place(phase.title, engine, pad + titleInset, y, width - 2 * pad - titleInset);
			y += phase.title.rect.h + itemGap;
			trailing = itemGap;
			if (phase.localCollapsed.value_or(phase.collapsed))
				continue;
		}
		for (auto& item : phase.items) {
			if (reminder && item.closed)
				continue;
			place(item.mark, engine, pad + markInset, y, size);
			place(item.label, engine, pad + labelInset, y, width - 2 * pad - labelInset, true);
			y += std::max(line, item.label.rect.h);
			if (!item.note.runs.empty()) {
				y += itemGap;
				place(item.note, engine, pad + labelInset, y, width - 2 * pad - labelInset, true);
				y += item.note.rect.h;
			}
			y += itemGap;
			trailing = itemGap;
		}
		if (!reminder) {
			y += gap;
			trailing += gap;
		}
	}
	cache.ownHeight = y - trailing + pad;
	cache.measuredWidth = width;
}

} // namespace

void forgetToolAgent(ToolAgentCache& cache, text::Engine& engine) {
	eachRow(cache, [&](Row& row) { engine.forget(row.runs); });
	cache = {};
}

void eachShownRow(const ToolAgentCache& cache,
                  const std::function<void(const text::Runs&, const text::Layout&, const SDL_FRect&)>& visit) {
	const auto row = [&](const Row& value) {
		if (value.rect.w > 0 && !value.runs.empty())
			visit(value.runs, value.layout, value.rect);
	};
	if (cache.mode == "hud") {
		row(cache.hud);
		return;
	}
	for (const auto* value : {&cache.mark, &cache.head, &cache.after, &cache.right, &cache.clock, &cache.intent,
	                          &cache.currentTool, &cache.retry})
		row(*value);
	for (const auto& chip : cache.chips)
		row(chip.label);
	for (const auto& phase : cache.phases) {
		row(phase.title);
		for (const auto& item : phase.items) {
			row(item.mark);
			row(item.label);
			row(item.note);
		}
	}
}

void syncToolAgent(const tsp::Node& node, ToolAgentCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	if (cache.synced && cache.revision == node.rev && cache.fontSize == fonts.size())
		return;
	const bool hovered = cache.headHovered, chromeless = cache.chromeless;
	std::vector<ToolAgentCache::Phase> previous;
	if (node.kind == tsp::Kind::Checklist) {
		previous = std::move(cache.phases);
		for (auto& phase : previous) {
			engine.forget(phase.title.runs);
			for (auto& item : phase.items) {
				engine.forget(item.mark.runs);
				engine.forget(item.label.runs);
				engine.forget(item.note.runs);
			}
		}
	}
	forgetToolAgent(cache, engine);
	cache.headHovered = hovered;
	cache.chromeless = chromeless;
	cache.running = node.str("status") == "running";
	cache.collapsible = node.flag("collapsible");
	if (node.kind == tsp::Kind::Checklist)
		syncChecklist(node, cache, previous);
	else {
		const auto [glyph, color] = statusMark(node.str("status", node.kind == tsp::Kind::Tool ? "done" : "pending"));
		cache.statusColor = color;
		add(cache.mark.runs, glyph, style(color));
		if (node.kind == tsp::Kind::Tool)
			syncTool(node, cache);
		else
			syncAgent(node, cache);
	}
	eachRow(cache, [&](Row& row) { prepare(row, engine); });
	updateClocks(node, cache, engine);
	cache.fontSize = fonts.size();
	cache.revision = node.rev;
	cache.synced = true;
	cache.dirty = true;
}

SDL_FPoint measureToolAgent(const tsp::Node& node, ToolAgentCache& cache, text::Engine& engine,
                            const text::Fonts& fonts, float width) {
	syncToolAgent(node, cache, engine, fonts);
	if (updateClocks(node, cache, engine))
		cache.dirty = true;
	width = std::max(0.0f, width);
	if (!cache.dirty && cache.width == width && cache.laidOutChromeless == cache.chromeless)
		return {cache.measuredWidth, cache.ownHeight + cache.bottomInset};
	cache.width = width;
	cache.laidOutChromeless = cache.chromeless;
	cache.childLeft = cache.childRight = cache.bottomInset = 0;
	cache.progressRect = {};
	const float size = fonts.size(), gap = size * gfx::space::sm, itemGap = size * gfx::space::xs;
	if (node.kind == tsp::Kind::Checklist)
		layoutChecklist(cache, engine, fonts, width);
	else if (node.kind == tsp::Kind::Tool) {
		const bool framed = !cache.inlineFrame && !cache.chromeless;
		cache.childLeft = cache.childRight = framed ? size * gfx::space::lg : 0;
		cache.bottomInset = framed ? size * gfx::space::lg : 0;
		const float inner = std::max(0.0f, width - cache.childLeft - cache.childRight);
		layoutHeader(cache, engine, fonts, cache.childLeft, cache.bottomInset, inner);
		float y = cache.bottomInset + cache.headRect.h;
		if (!cache.intent.runs.empty()) {
			y += gap;
			place(cache.intent, engine, cache.childLeft, y, inner);
			y += cache.intent.rect.h;
		}
		cache.ownHeight = y + (node.children.empty() ? 0 : gap);
		cache.measuredWidth = width;
	} else {
		const float indent = std::min(width, cache.indent * size);
		const float pad = cache.chromeless ? 0 : size * gfx::space::lg;
		cache.childLeft = indent + pad + size + gap;
		cache.childRight = cache.bottomInset = pad;
		layoutHeader(cache, engine, fonts, indent + pad, pad, std::max(0.0f, width - indent - pad * 2));
		const float inner = std::max(0.0f, width - cache.childLeft - cache.childRight);
		float y = pad + cache.headRect.h;
		if (!cache.currentTool.runs.empty()) {
			y += gap;
			const float ageWidth = std::min(inner, cache.currentToolAge.naturalWidth);
			place(cache.currentTool, engine, cache.childLeft, y, inner - ageWidth - (ageWidth > 0 ? gap : 0));
			place(cache.currentToolAge, engine, width - cache.childRight - ageWidth, y, ageWidth);
			y += std::max(cache.currentTool.rect.h, cache.currentToolAge.rect.h);
		}
		if (!cache.retry.runs.empty()) {
			y += cache.currentTool.runs.empty() ? gap : itemGap;
			const float ageWidth = std::min(inner, cache.retryClock.naturalWidth);
			place(cache.retry, engine, cache.childLeft, y, inner - ageWidth - gap);
			place(cache.retryClock, engine, width - cache.childRight - ageWidth, y, ageWidth);
			y += std::max(cache.retry.rect.h, cache.retryClock.rect.h);
		}
		if (cache.progress >= 0) {
			y += cache.currentTool.runs.empty() && cache.retry.runs.empty() ? gap : itemGap;
			cache.progressRect = {cache.childLeft, y, inner * 0.5f, size * 0.2f};
			y += cache.progressRect.h;
		}
		cache.ownHeight = y + (node.children.empty() ? 0 : gap);
		cache.measuredWidth = width;
	}
	cache.dirty = false;
	return {cache.measuredWidth, cache.ownHeight + cache.bottomInset};
}

bool toggleChecklistPhase(ToolAgentCache& cache, std::string_view item) {
	for (auto& phase : cache.phases)
		if (phase.id == item) {
			phase.localCollapsed = !phase.localCollapsed.value_or(phase.collapsed);
			cache.dirty = true;
			return true;
		}
	return false;
}

bool paintToolAgent(const tsp::Node& node, ToolAgentCache& cache, text::Engine& engine, const text::Fonts& fonts,
                    SDL_Renderer& renderer, const SDL_FRect& rect, bool collapsed, view::HitTargets& targets) {
	measureToolAgent(node, cache, engine, fonts, rect.w);
	gfx::ScopedClip outer(renderer, rect);
	const float size = fonts.size(), line = fonts.lineHeight(), unit = size / 15;
	const float pillPadX = size * gfx::space::md, pillPadY = size * gfx::space::xs;
	const float chevronSlot = size + size * gfx::space::sm;
	bool animating = false;
	auto draw = [&](const Row& row) {
		animating |= drawRow(row, rect, engine, targets, outer.visible);
	};
	if (node.kind == tsp::Kind::Checklist) {
		if (cache.mode == "hud") {
			const auto pill = translated(cache.headRect, rect);
			gfx::rounded(renderer, pill, pill.h * 0.5f, tint(text::palette::accent, 24));
			gfx::roundedBorder(renderer, pill, pill.h * 0.5f, tint(text::palette::accent, 75), unit);
			// The hover card shows the checklist itself (view/pointer.cpp).
			targets.add(pill, view::TargetKind::Hover);
			draw(cache.hud);
			return animating;
		}
		const bool reminder = cache.mode == "reminder";
		if (reminder) {
			gfx::rounded(renderer, rect, size * 0.35f, tint(text::palette::warning, 12));
			gfx::roundedBorder(renderer, rect, size * 0.35f, tint(text::palette::warning, 65), unit);
		}
		draw(cache.after);
		for (const auto& phase : cache.phases) {
			if (!reminder) {
				const bool folded = phase.localCollapsed.value_or(phase.collapsed);
				const auto title = translated(phase.title.rect, rect);
				targets.add({rect.x, title.y, rect.w, line}, view::TargetKind::Phase, phase.id);
				chevron(renderer, {rect.x + size * 0.45f, title.y + line * 0.5f}, size, folded);
				draw(phase.title);
				if (folded)
					continue;
			}
			for (const auto& item : phase.items) {
				if (reminder && item.closed)
					continue;
				draw(item.mark);
				draw(item.label);
				draw(item.note);
			}
		}
		return animating;
	}
	if (node.kind == tsp::Kind::Tool && !cache.inlineFrame && !cache.chromeless) {
		gfx::rounded(renderer, rect, size * (8.0f / 15), text::palette::surface);
		gfx::roundedBorder(renderer, rect, size * (8.0f / 15),
		                   cache.running                   ? tint(text::palette::accent, 140)
		                   : node.str("status") == "error" ? tint(text::palette::error, 153)
		                                                   : text::palette::border,
		                   unit);
	}
	const auto head = translated(cache.headRect, rect);
	if (cache.collapsible)
		targets.add(head, view::TargetKind::Toggle);
	if (!cache.completedIntent.empty())
		targets.add(head, view::TargetKind::Hover, cache.completedIntent);
	if (cache.collapsible)
		targets.add({head.x + head.w - chevronSlot, head.y, chevronSlot, head.h}, view::TargetKind::Toggle);
	// Link and button hits follow the row-wide toggle/tooltip targets.
	if (cache.running && gfx::intersects(head, outer.visible)) {
		animating = true;
		const SDL_FPoint center{head.x + size * 0.48f, head.y + head.h * 0.5f};
		const float angle = static_cast<float>(SDL_GetTicks() % 1200) / 1200 * pi * 2;
		gfx::arc(renderer, center, size * 0.33f, 0, pi * 2, tint(cache.statusColor, 45), unit * 1.6f);
		gfx::arc(renderer, center, size * 0.33f, angle, angle + pi * 1.35f, cache.statusColor, unit * 1.6f);
	} else
		draw(cache.mark);
	{
		const auto contentRect = translated(cache.headContentRect, rect);
		gfx::ScopedClip content(renderer, contentRect);
		animating |= drawRow(cache.head, rect, engine, targets, content.visible);
		for (const auto& chip : cache.chips) {
			const auto label = translated(chip.label.rect, rect);
			const SDL_FRect pill{label.x - pillPadX, label.y - pillPadY, label.w + pillPadX * 2,
			                     label.h + pillPadY * 2};
			gfx::rounded(renderer, pill, line * 0.25f, tint(chip.color, 25));
			if (!chip.tip.empty())
				targets.add(gfx::intersect(pill, contentRect), view::TargetKind::Hover, chip.tip);
			animating |= drawRow(chip.label, rect, engine, targets, content.visible);
		}
		animating |= drawRow(cache.after, rect, engine, targets, content.visible);
		if (node.kind == tsp::Kind::Tool && cache.headHovered)
			for (const auto& button : cache.buttons) {
				const auto label = translated(button.label.rect, rect);
				const SDL_FRect buttonRect{label.x - pillPadX, label.y - pillPadY, label.w + pillPadX * 2,
				                           label.h + pillPadY * 2};
				gfx::rounded(renderer, buttonRect, size * 0.25f, text::palette::surface);
				gfx::rounded(renderer, buttonRect, size * 0.25f,
				             button.on ? tint(text::palette::accent, 55) : text::palette::surface2);
				gfx::roundedBorder(renderer, buttonRect, size * 0.25f,
				                   button.primary  ? text::palette::accent
				                   : button.danger ? tint(text::palette::error, 140)
				                                   : text::palette::border,
				                   unit);
				if (!button.disabled)
					targets.add(gfx::intersect(buttonRect, contentRect),
					            button.copy ? view::TargetKind::Copy : view::TargetKind::Action,
					            button.copy ? std::string_view(cache.command) : std::string_view(button.id));
				else
					targets.add(gfx::intersect(buttonRect, contentRect), view::TargetKind::Hover, button.tip);
				animating |= drawRow(button.label, rect, engine, targets, content.visible);
			}
	}
	draw(cache.right);
	draw(cache.clock);
	if (cache.collapsible)
		chevron(renderer, {head.x + head.w - size * 0.5f, head.y + head.h * 0.5f}, size, collapsed);
	draw(cache.intent);
	draw(cache.currentTool);
	draw(cache.currentToolAge);
	draw(cache.retry);
	draw(cache.retryClock);
	if (cache.progress >= 0) {
		const auto track = translated(cache.progressRect, rect);
		gfx::rounded(renderer, track, track.h * 0.5f, text::palette::surface2);
		gfx::rounded(renderer, {track.x, track.y, track.w * cache.progress, track.h}, track.h * 0.5f,
		             text::palette::accent);
	}
	return animating || (cache.clocked && gfx::intersects(translated(cache.clock.rect, rect), outer.visible))
	    || (cache.toolClocked && gfx::intersects(translated(cache.currentToolAge.rect, rect), outer.visible))
	    || (cache.retryClocked && cache.retrySecond > 0
	        && gfx::intersects(translated(cache.retryClock.rect, rect), outer.visible));
}

} // namespace kinds
