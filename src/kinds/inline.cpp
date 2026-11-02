#include "inline.h"

#include "gfx/draw.h"
#include "gfx/icons.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>

namespace kinds {
namespace {

constexpr float naturalLimit = 1000000;
constexpr float pi = std::numbers::pi_v<float>;
constexpr std::string_view elapsedAlphabet = "0123456789:.smh ";
constexpr std::array<std::string_view, 10> spinnerFrames{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

text::RunStyle style(SDL_Color color, text::FontId font = text::FontId::Ui) {
	text::RunStyle result;
	result.color = color;
	result.font = font;
	return result;
}

SDL_Color tone(std::string_view name, SDL_Color fallback) {
	auto result = style(fallback);
	text::applyTokens(name, result);
	return result.color;
}

void layoutText(InlineText& text, text::Engine& engine, float width = naturalLimit) {
	width = std::max(0.0f, width);
	if (text.width == width)
		return;
	text.width = width;
	text.layout = engine.layout(text.runs, {.width = width, .wrap = text::Wrap::None, .maxLines = 1});
}

void syncText(InlineText& text, text::Runs runs, text::Engine& engine) {
	text.runs = std::move(runs);
	layoutText(text, engine);
	text.naturalWidth = text.layout.width;
}

void glyph(InlineText& text, std::string value, text::RunStyle base, text::Engine& engine) {
	text::Runs runs;
	runs.push_back({std::move(value), std::move(base)});
	syncText(text, std::move(runs), engine);
}

bool animatedText(const InlineText& text) {
	return std::any_of(text.runs.begin(), text.runs.end(), [](const auto& run) { return run.style.fx != 0; });
}

bool animatedTextVisible(const InlineText& text, float x, float y, const SDL_FRect& visible) {
	for (const auto& piece : text.layout.pieces) {
		if (text.runs[piece.run].style.fx != 0 && piece.width > 0
		    && gfx::intersects({x + piece.x, y, piece.width, text.layout.lineHeight}, visible))
			return true;
	}
	return false;
}

void drawText(const InlineText& text, text::Engine& engine, float x, float y, view::HitTargets& hits) {
	engine.draw(text.runs, text.layout, x, y);
	for (const auto& piece : text.layout.pieces) {
		const auto& href = text.runs[piece.run].style.href;
		if (!href.empty())
			hits.add({x + piece.x, y, piece.width, text.layout.lineHeight}, view::TargetKind::Link, href);
	}
}

void wake(InlineCache& cache, Uint64 tick) {
	if (cache.nextWake == 0 || tick < cache.nextWake)
		cache.nextWake = tick;
}

float elapsedWidth(const InlineCache& cache) {
	float width = 0;
	for (const char c : cache.elapsedText) {
		if (c == '\0')
			break;
		width += cache.glyphs[elapsedAlphabet.find(c)].naturalWidth;
	}
	return width;
}

void updateElapsed(const tsp::Node& node, InlineCache& cache, Uint64 now) {
	if (cache.textWake != 0 && now < cache.textWake)
		return;
	if (cache.stopped && cache.elapsedText.front() != '\0')
		return;
	const double ms = std::max(0.0, cache.stopped ? cache.stoppedAt : cache.age + node.since("age"));
	const auto seconds = static_cast<long long>(ms / 1000);
	if (cache.clock) {
		if (seconds >= 3600)
			std::snprintf(cache.elapsedText.data(), cache.elapsedText.size(), "%lld:%02lld:%02lld", seconds / 3600,
			              (seconds / 60) % 60, seconds % 60);
		else
			std::snprintf(cache.elapsedText.data(), cache.elapsedText.size(), "%02lld:%02lld", seconds / 60,
			              seconds % 60);
	} else if (ms < 1000) {
		// Truncation makes the displayed tenth and its next deadline agree.
		std::snprintf(cache.elapsedText.data(), cache.elapsedText.size(), "0.%llds", static_cast<long long>(ms / 100));
	} else if (seconds < 60) {
		std::snprintf(cache.elapsedText.data(), cache.elapsedText.size(), "%llds", seconds);
	} else if (seconds < 3600) {
		std::snprintf(cache.elapsedText.data(), cache.elapsedText.size(), "%lldm %llds", seconds / 60, seconds % 60);
	} else {
		std::snprintf(cache.elapsedText.data(), cache.elapsedText.size(), "%lldh %lldm", seconds / 3600,
		              (seconds / 60) % 60);
	}
	const float width = elapsedWidth(cache);
	if (width != cache.naturalWidth) {
		cache.naturalWidth = width;
		cache.width = -1;
		cache.sizeChanged = true;
	}
	if (!cache.stopped) {
		const double quantum = !cache.clock && ms < 1000 ? 100 : !cache.clock && ms >= 3600000 ? 60000 : 1000;
		cache.textWake = now + static_cast<Uint64>(std::max(1.0, std::ceil(quantum - std::fmod(ms, quantum))));
	}
}

void hatch(SDL_Renderer& renderer, const SDL_FRect& rect, SDL_Color color, float unit) {
	if (rect.w <= 0 || rect.h <= 0)
		return;
	color.a = static_cast<Uint8>(static_cast<float>(color.a) * 0.65f);
	for (float at = -rect.h; at < rect.w; at += unit * 4) {
		const float from = std::max(0.0f, at), to = std::min(rect.w, at + rect.h);
		gfx::line(renderer, {rect.x + from, rect.y + rect.h - (from - at)}, {rect.x + to, rect.y + rect.h - (to - at)},
		          color, unit);
	}
}

void partFill(SDL_Renderer& renderer, const SDL_FRect& rect, const InlinePart& part, float unit) {
	auto color = part.color;
	if (part.hatch)
		color.a = static_cast<Uint8>(static_cast<float>(color.a) * 0.45f);
	gfx::fill(renderer, rect, color);
	if (part.hatch)
		hatch(renderer, rect, part.color, unit);
}

float markPosition(const InlineMark& mark, float track, float gap) {
	const float half = std::min(track * 0.5f, mark.glyph.naturalWidth * 0.5f + gap);
	return std::clamp(track * mark.at, half, track - half);
}

void barSpan(const InlineCache& cache, SDL_Renderer& renderer, const SDL_FRect& track, float begin, float end,
             float unit) {
	if (end <= begin)
		return;
	const SDL_FRect span{track.x + begin, track.y, end - begin, track.h};
	gfx::rounded(renderer, span, track.h * 0.5f, text::palette::surface2);
	if (cache.hasParts) {
		for (const auto& part : cache.parts) {
			const float left = std::max(begin, track.w * part.from), right = std::min(end, track.w * part.to);
			if (right > left)
				partFill(renderer, {track.x + left, track.y, right - left, track.h}, part, unit);
		}
	} else if (cache.known && cache.value > 0) {
		const float right = std::min(end, std::max(track.h, track.w * cache.value));
		if (right > begin)
			gfx::rounded(renderer, {span.x, span.y, right - begin, span.h}, track.h * 0.5f, cache.color);
	}
}

void paintMeter(const InlineCache& cache, const text::Fonts& fonts, text::Engine& engine, SDL_Renderer& renderer,
                const SDL_FRect& rect, view::HitTargets& hits) {
	const float unit = fonts.size() / 15;
	const float gap = fonts.size() * gfx::space::sm;
	const float left = rect.x + cache.trackOffset,
	            centerY = rect.y + (rect.h - cache.height) * 0.5f + cache.height * 0.5f;
	if (cache.meterStyle == InlineCache::MeterStyle::Bar) {
		const SDL_FRect track{left, centerY - cache.thickness * 0.5f, cache.trackWidth, cache.thickness};
		float begin = 0;
		// Marks are sorted once at sync; icon gaps really break the track, not paint over it.
		for (const auto& mark : cache.marks) {
			if (mark.glyph.runs.empty())
				continue;
			const float at = markPosition(mark, track.w, gap), half = mark.glyph.naturalWidth * 0.5f + gap;
			barSpan(cache, renderer, track, begin, std::max(begin, at - half), unit);
			begin = std::max(begin, std::min(track.w, at + half));
		}
		barSpan(cache, renderer, track, begin, track.w, unit);
		for (const auto& part : cache.parts)
			if (!part.label.empty())
				hits.add(
				    {left + cache.trackWidth * part.from, rect.y, cache.trackWidth * (part.to - part.from), rect.h},
				    view::TargetKind::Hover, part.label);
	} else if (cache.meterStyle == InlineCache::MeterStyle::Ring) {
		const float radius = std::max(0.0f, cache.trackWidth * 0.5f - unit * 1.5f);
		const SDL_FPoint center{left + cache.trackWidth * 0.5f, centerY};
		gfx::arc(renderer, center, radius, 0, pi * 2, text::palette::surface2, unit * 3);
		if (cache.hasParts) {
			for (const auto& part : cache.parts) {
				const float from = -pi * 0.5f + part.from * pi * 2, to = -pi * 0.5f + part.to * pi * 2;
				if (!part.hatch)
					gfx::arc(renderer, center, radius, from, to, part.color, unit * 3);
				else
					for (float at = from; at < to; at += 0.26f)
						gfx::arc(renderer, center, radius, at, std::min(to, at + 0.15f), part.color, unit * 3);
			}
		} else if (cache.known && cache.value > 0) {
			gfx::arc(renderer, center, radius, -pi * 0.5f, -pi * 0.5f + cache.value * pi * 2, cache.color, unit * 3);
		}
	} else {
		const float gap = std::min(fonts.size() * gfx::space::xs, cache.trackWidth / static_cast<float>(cache.steps));
		const float side = std::max(0.0f, (cache.trackWidth - static_cast<float>(cache.steps - 1) * gap)
		                                      / static_cast<float>(cache.steps));
		const int on = static_cast<int>(std::round(cache.value * static_cast<float>(cache.steps)));
		for (int i = 0; i < cache.steps; ++i) {
			const SDL_FRect cell{left + static_cast<float>(i) * (side + gap), centerY - side * 0.5f, side, side};
			gfx::rounded(renderer, cell, unit * 1.5f,
			             !cache.hasParts && cache.known && i < on ? cache.color : text::palette::surface2);
			if (cache.hasParts)
				for (const auto& part : cache.parts) {
					const int first = static_cast<int>(std::round(part.from * static_cast<float>(cache.steps)));
					const int last = static_cast<int>(std::round(part.to * static_cast<float>(cache.steps)));
					if (i < first || i >= last)
						continue;
					partFill(renderer, cell, part, unit);
					if (!part.label.empty())
						hits.add(cell, view::TargetKind::Hover, part.label);
					break;
				}
		}
	}
	for (const auto& mark : cache.marks) {
		float x = left + cache.trackWidth * mark.at, y = centerY;
		SDL_FPoint from{x, y - cache.thickness * 0.5f - unit * 3}, to{x, y + cache.thickness * 0.5f + unit * 3};
		if (cache.meterStyle == InlineCache::MeterStyle::Ring) {
			const float angle = -pi * 0.5f + mark.at * pi * 2;
			const float radius = std::max(0.0f, cache.trackWidth * 0.5f - unit * 1.5f);
			const SDL_FPoint center{left + cache.trackWidth * 0.5f, centerY};
			x = center.x + std::cos(angle) * radius;
			y = center.y + std::sin(angle) * radius;
			from = {center.x + std::cos(angle) * (radius - unit * 3), center.y + std::sin(angle) * (radius - unit * 3)};
			to = {center.x + std::cos(angle) * (radius + unit * 3), center.y + std::sin(angle) * (radius + unit * 3)};
		} else if (!mark.glyph.runs.empty())
			x = left + markPosition(mark, cache.trackWidth, gap);
		if (!mark.glyph.runs.empty())
			engine.draw(mark.glyph.runs, mark.glyph.layout, x - mark.glyph.naturalWidth * 0.5f,
			            y - mark.glyph.layout.height * 0.5f);
		else
			gfx::line(renderer, from, to, mark.color, unit * 1.5f);
		if (!mark.title.empty()) {
			const float hitWidth = std::max(unit * 8, mark.glyph.naturalWidth + unit * 4);
			hits.add({x - hitWidth * 0.5f, cache.meterStyle == InlineCache::MeterStyle::Ring ? y - unit * 5 : rect.y,
			          hitWidth, cache.meterStyle == InlineCache::MeterStyle::Ring ? unit * 10 : rect.h},
			         view::TargetKind::Hover, mark.title);
		}
	}
	const float top = rect.y + (rect.h - cache.height) * 0.5f;
	drawText(cache.label, engine, rect.x + cache.labelOffset, top + (cache.height - cache.label.layout.height) * 0.5f,
	         hits);
	drawText(cache.total, engine, rect.x + cache.totalOffset, top + (cache.height - cache.total.layout.height) * 0.5f,
	         hits);
}

void paintEffort(const InlineCache& cache, const text::Fonts& fonts, SDL_Renderer& renderer, const SDL_FRect& rect,
                 Uint64 now) {
	const float unit = fonts.size() / 15, radius = fonts.size() * 0.42f;
	const SDL_FPoint center{rect.x + cache.naturalWidth * 0.5f, rect.y + rect.h * 0.5f};
	if (cache.rung < 0) {
		for (int i = 0; i < 12; i += 2)
			gfx::arc(renderer, center, radius, static_cast<float>(i) * pi / 6, static_cast<float>(i + 1) * pi / 6,
			         text::palette::muted, unit * 1.5f);
	} else if (cache.rung == 6) {
		const float flicker = 0.8f + 0.2f * static_cast<float>(std::sin(static_cast<double>(now) * 0.014));
		const float outer = radius * flicker, inner = outer * 0.55f;
		gfx::rounded(renderer, {center.x - outer, center.y - outer, outer * 2, outer * 2}, outer, text::palette::error);
		gfx::rounded(renderer, {center.x - inner, center.y + radius * 0.2f - inner, inner * 2, inner * 2}, inner,
		             text::palette::warning);
	} else {
		gfx::arc(renderer, center, radius, 0, pi * 2, text::palette::surface2, unit * 2);
		if (cache.rung > 0)
			gfx::arc(renderer, center, radius, -pi * 0.5f, -pi * 0.5f + static_cast<float>(cache.rung) / 6 * pi * 2,
			         text::palette::pending, unit * 2.5f);
	}
}

} // namespace

std::string keyCap(std::string_view key) {
	std::string lower(key);
	for (char& c : lower)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#if defined(__APPLE__)
	if (lower == "ctrl" || lower == "control")
		return "⌃";
	if (lower == "alt" || lower == "option")
		return "⌥";
	if (lower == "cmd" || lower == "command" || lower == "meta" || lower == "super" || lower == "mod")
		return "⌘";
#else
	if (lower == "ctrl" || lower == "control" || lower == "mod")
		return "Ctrl";
	if (lower == "alt" || lower == "option")
		return "Alt";
	if (lower == "cmd" || lower == "command" || lower == "meta" || lower == "super") {
#if defined(_WIN32)
		return "Win";
#else
		return "Super";
#endif
	}
#endif
	if (lower == "shift")
		return "⇧";
	if (lower == "enter" || lower == "return")
		return "⏎";
	if (lower == "tab")
		return "⇥";
	if (lower == "escape" || lower == "esc")
		return "Esc";
	if (lower == "up" || lower == "arrowup")
		return "↑";
	if (lower == "down" || lower == "arrowdown")
		return "↓";
	if (lower == "left" || lower == "arrowleft")
		return "←";
	if (lower == "right" || lower == "arrowright")
		return "→";
	if (lower == "backspace")
		return "⌫";
#if defined(__APPLE__)
	if (lower == "delete" || lower == "del")
		return "⌦";
	if (lower == "pageup" || lower == "page_up" || lower == "pgup")
		return "⇞";
	if (lower == "pagedown" || lower == "page_down" || lower == "pgdn")
		return "⇟";
	if (lower == "home")
		return "↖";
	if (lower == "end")
		return "↘";
#else
	if (lower == "delete" || lower == "del")
		return "Del";
	if (lower == "pageup" || lower == "page_up" || lower == "pgup")
		return "PgUp";
	if (lower == "pagedown" || lower == "page_down" || lower == "pgdn")
		return "PgDn";
	if (lower == "home")
		return "Home";
	if (lower == "end")
		return "End";
#endif
	if (lower == "space" || lower == " ")
		return "Space";
	if (lower == "insert" || lower == "ins")
		return "Ins";
	if (lower == "capslock")
		return "Caps";
	if (lower.size() == 1 || (lower.size() > 1 && lower.front() == 'f' && lower[1] >= '0' && lower[1] <= '9')) {
		for (char& c : lower)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return lower;
	}
	return std::string(key);
}

bool inlineKind(tsp::Kind kind) {
	switch (kind) {
	case tsp::Kind::Badge:
	case tsp::Kind::Kbd:
	case tsp::Kind::Icon:
	case tsp::Kind::Spinner:
	case tsp::Kind::Elapsed:
	case tsp::Kind::Progress:
	case tsp::Kind::Rate:
	case tsp::Kind::Meter:
	case tsp::Kind::Effort:
		return true;
	default:
		return false;
	}
}

void syncInline(const tsp::Node& node, InlineCache& cache, text::Engine& engine, const text::Fonts& fonts) {
	cache = {};
	const float unit = fonts.size() / 15;
	cache.height = fonts.lineHeight();
	cache.color = tone(node.str("tone"), text::palette::muted);
	switch (node.kind) {
	case tsp::Kind::Badge:
		cache.pad = fonts.size() * gfx::space::md;
		syncText(cache.text, text::toRuns(node.prop("text"), style(cache.color)), engine);
		cache.naturalWidth = cache.text.naturalWidth + cache.pad * 2;
		cache.height = std::max(cache.height, cache.text.layout.height) + fonts.size() * gfx::space::xs * 2;
		break;
	case tsp::Kind::Icon: {
		auto value = gfx::iconGlyph(node.str("name"));
		const bool known = !value.empty();
		glyph(cache.text, known ? std::move(value) : "•",
		      style(cache.color, known ? text::FontId::Icons : text::FontId::Ui), engine);
		cache.naturalWidth = cache.text.naturalWidth;
		cache.height = std::max(cache.height, cache.text.layout.height);
		break;
	}
	case tsp::Kind::Kbd: {
		cache.send = node.str("role") == "omp.composer.send";
		cache.pad = cache.send ? 0 : fonts.size() * gfx::space::md;
		const float gap = fonts.size() * gfx::space::sm;
		const auto& keys = node.prop("keys");
		if (keys.is_array())
			for (const auto& key : keys) {
				if (!key.is_string())
					continue;
				const size_t index = cache.text.runs.size();
				cache.text.runs.push_back({keyCap(key.get_ref<const std::string&>()), style(cache.color)});
				const auto& value = cache.text.runs.back().text;
				const float width = fonts.width(text::FontId::Ui, value);
				cache.text.layout.pieces.push_back({index, 0, value.size(), cache.keyWidth + cache.pad, width});
				cache.keyWidth += width + cache.pad * 2 + gap;
			}
		if (!cache.text.runs.empty())
			cache.keyWidth -= gap;
		cache.text.layout.width = cache.keyWidth;
		cache.text.layout.height = cache.text.layout.lineHeight = cache.height;
		cache.text.layout.ascent = static_cast<float>(TTF_GetFontAscent(&fonts.get(text::FontId::Ui)));
		cache.height += fonts.size() * gfx::space::xs * 2;
		cache.naturalWidth =
		    cache.send
		        ? std::max({fonts.size() * 2.2f, cache.keyWidth + fonts.size() * gfx::space::xs * 2, cache.height})
		        : cache.keyWidth;
		if (cache.send)
			cache.height = cache.naturalWidth;
		break;
	}
	case tsp::Kind::Spinner:
		cache.glyphs.resize(spinnerFrames.size());
		for (size_t i = 0; i < spinnerFrames.size(); ++i) {
			glyph(cache.glyphs[i], std::string(spinnerFrames[i]), style(text::palette::accent), engine);
			cache.trackNaturalWidth = std::max(cache.trackNaturalWidth, cache.glyphs[i].naturalWidth);
			cache.height = std::max(cache.height, cache.glyphs[i].layout.height);
		}
		syncText(cache.label, text::toRuns(node.prop("label"), style(text::palette::muted)), engine);
		cache.naturalWidth = cache.trackNaturalWidth
		                   + (cache.label.runs.empty() ? 0 : fonts.size() * gfx::space::sm + cache.label.naturalWidth);
		cache.height = std::max(cache.height, cache.label.layout.height);
		break;
	case tsp::Kind::Elapsed:
		cache.age = node.num("age");
		cache.stopped = node.prop("stopped").is_number();
		cache.stoppedAt = node.num("stopped");
		cache.clock = node.str("format") == "clock";
		cache.glyphs.resize(elapsedAlphabet.size());
		for (size_t i = 0; i < elapsedAlphabet.size(); ++i)
			glyph(cache.glyphs[i], std::string(1, elapsedAlphabet[i]), style(text::palette::muted, text::FontId::Mono),
			      engine);
		updateElapsed(node, cache, SDL_GetTicks());
		cache.sizeChanged = false;
		break;
	case tsp::Kind::Progress:
		cache.known = node.prop("value").is_number();
		cache.value = std::clamp(static_cast<float>(node.num("value")), 0.0f, 1.0f);
		cache.color = text::palette::accent;
		cache.thickness = unit * 6;
		cache.trackNaturalWidth = fonts.size() * 3;
		syncText(cache.label, text::toRuns(node.prop("label"), style(text::palette::muted)), engine);
		cache.naturalWidth = fonts.size() * 12
		                   + (cache.label.runs.empty() ? 0 : fonts.size() * gfx::space::sm + cache.label.naturalWidth);
		cache.height = std::max(cache.height, cache.label.layout.height);
		break;
	case tsp::Kind::Rate: {
		std::array<char, 64> value{};
		std::snprintf(value.data(), value.size(), "%.1f", node.num("value"));
		std::string label(value.data());
		if (!node.str("unit").empty()) {
			label += ' ';
			label += node.str("unit");
		}
		glyph(cache.text, std::move(label), style(text::palette::muted, text::FontId::Mono), engine);
		cache.naturalWidth = cache.text.naturalWidth;
		cache.height = std::max(cache.height, cache.text.layout.height);
		break;
	}
	case tsp::Kind::Meter: {
		cache.context = node.str("role") == "omp.composer.context";
		cache.known = node.prop("value").is_number();
		cache.value = std::clamp(static_cast<float>(node.num("value")), 0.0f, 1.0f);
		const auto meterStyle = node.str("style", "bar");
		cache.meterStyle = meterStyle == "ring"   ? InlineCache::MeterStyle::Ring
		                 : meterStyle == "blocks" ? InlineCache::MeterStyle::Blocks
		                                          : InlineCache::MeterStyle::Bar;
		cache.steps = std::max(1, static_cast<int>(node.num("steps", 10)));
		const auto meterSize = node.str("size", "sm");
		cache.thickness = cache.context ? unit : (meterSize == "lg" ? 8 : meterSize == "md" ? 6 : 4) * unit;
		SDL_Color color = text::palette::accent;
		const auto& thresholds = node.prop("thresholds");
		if (thresholds.is_object() && cache.known) {
			if (const auto bad = thresholds.find("bad");
			    bad != thresholds.end() && bad->is_number() && cache.value >= bad->get<float>())
				color = text::palette::error;
			else if (const auto warn = thresholds.find("warn");
			         warn != thresholds.end() && warn->is_number() && cache.value >= warn->get<float>())
				color = text::palette::warning;
		}
		cache.color = tone(node.str("tone"), color);
		const auto& parts = node.prop("parts");
		cache.hasParts = parts.is_array();
		const std::array colors{text::palette::accent, text::palette::pending, text::palette::info,
		                        text::palette::success, text::palette::warning};
		float used = 0;
		if (cache.hasParts)
			for (size_t i = 0; i < parts.size(); ++i) {
				const auto& part = parts[i];
				const float end = std::min(1.0f, used + std::clamp(part.value("value", 0.0f), 0.0f, 1.0f));
				const auto token = part.value("token", "");
				const auto partColor =
				    token == "track" ? text::palette::surface2 : tone(token, colors[i % colors.size()]);
				cache.parts.push_back({used, end, partColor, part.value("hatch", false), part.value("label", "")});
				used = end;
			}
		const auto& marks = node.prop("marks");
		if (marks.is_array())
			for (const auto& mark : marks) {
				InlineMark item;
				item.at = std::clamp(mark.value("at", 0.0f), 0.0f, 1.0f);
				item.color = tone(mark.value("tone", ""), text::palette::muted);
				item.title = mark.value("title", "");
				if (auto value = gfx::iconGlyph(mark.value("icon", "")); !value.empty()) {
					glyph(item.glyph, std::move(value), style(item.color, text::FontId::Icons), engine);
					item.glyph.layout.height = item.glyph.layout.lineHeight = fonts.lineHeight();
					item.glyph.layout.ascent = static_cast<float>(TTF_GetFontAscent(&fonts.get(text::FontId::Ui)));
				}
				cache.marks.push_back(std::move(item));
			}
		std::stable_sort(cache.marks.begin(), cache.marks.end(),
		                 [](const auto& a, const auto& b) { return a.at < b.at; });
		syncText(cache.label, text::toRuns(node.prop("label"), style(text::palette::muted)), engine);
		syncText(cache.total, text::toRuns(node.prop("total"), style(text::palette::dim)), engine);
		cache.height = std::max({cache.height, cache.label.layout.height, cache.total.layout.height});
		if (cache.meterStyle == InlineCache::MeterStyle::Ring)
			cache.trackNaturalWidth = fonts.lineHeight() * 0.76f + unit * 3;
		else if (cache.meterStyle == InlineCache::MeterStyle::Blocks)
			cache.trackNaturalWidth = static_cast<float>(cache.steps) * fonts.size() * 0.45f
			                        + static_cast<float>(cache.steps - 1) * fonts.size() * gfx::space::xs;
		else
			cache.trackNaturalWidth = fonts.size() * 3;
		const int labels = static_cast<int>(!cache.label.runs.empty()) + static_cast<int>(!cache.total.runs.empty());
		cache.naturalWidth = cache.trackNaturalWidth + cache.label.naturalWidth + cache.total.naturalWidth
		                   + fonts.size() * gfx::space::sm * static_cast<float>(labels);
		if (cache.meterStyle == InlineCache::MeterStyle::Bar)
			cache.naturalWidth = std::max(cache.naturalWidth, fonts.size() * 40);
		break;
	}
	case tsp::Kind::Effort: {
		constexpr std::array<std::string_view, 7> levels{"off", "minimal", "low", "medium", "high", "xhigh", "max"};
		const auto found = std::find(levels.begin(), levels.end(), node.str("level"));
		cache.rung = found == levels.end() ? -1 : static_cast<int>(found - levels.begin());
		cache.naturalWidth = fonts.size() * 0.84f + unit * 2.5f;
		cache.height = std::max(cache.height, cache.naturalWidth);
		break;
	}
	default:
		break;
	}
	cache.textAnimated = animatedText(cache.text) || animatedText(cache.label) || animatedText(cache.total);
}

SDL_FPoint measureInline(const tsp::Node& node, InlineCache& cache, text::Engine& engine, const text::Fonts& fonts,
                         float width) {
	if (node.kind == tsp::Kind::Elapsed)
		updateElapsed(node, cache, SDL_GetTicks());
	width = std::max(0.0f, width);
	const bool fill = node.kind == tsp::Kind::Progress
	               || (node.kind == tsp::Kind::Meter && cache.meterStyle == InlineCache::MeterStyle::Bar);
	const float available = width >= naturalLimit ? cache.naturalWidth
	                      : fill                  ? width
	                                              : std::min(width, cache.naturalWidth);
	if (cache.width == available)
		return {available, cache.height};
	cache.width = available;
	if (node.kind == tsp::Kind::Meter || node.kind == tsp::Kind::Progress) {
		const float gap = fonts.size() * gfx::space::sm;
		const float labelGap = cache.label.runs.empty() ? 0 : std::min(gap, available * 0.25f);
		const float totalGap = cache.total.runs.empty() ? 0 : std::min(gap, available * 0.25f);
		float budget = std::max(0.0f, available - std::min(cache.trackNaturalWidth, available) - labelGap - totalGap);
		layoutText(cache.total, engine, std::min(cache.total.naturalWidth, budget));
		budget = std::max(0.0f, budget - cache.total.layout.width);
		layoutText(cache.label, engine, std::min(cache.label.naturalWidth, budget));
		const float room =
		    std::max(0.0f, available - cache.label.layout.width - cache.total.layout.width - labelGap - totalGap);
		cache.trackWidth = fill ? room : std::min(cache.trackNaturalWidth, room);
		cache.trackOffset = cache.context && !cache.label.runs.empty() ? cache.label.layout.width + labelGap : 0;
		cache.labelOffset = cache.context ? 0 : cache.trackWidth + labelGap;
		cache.totalOffset = cache.context ? cache.trackOffset + cache.trackWidth + totalGap
		                                  : cache.labelOffset + cache.label.layout.width + totalGap;
	} else if (node.kind == tsp::Kind::Spinner) {
		cache.labelOffset = std::min(available, cache.trackNaturalWidth
		                                            + (cache.label.runs.empty() ? 0 : fonts.size() * gfx::space::sm));
		layoutText(cache.label, engine, std::max(0.0f, available - cache.labelOffset));
	} else if (node.kind == tsp::Kind::Badge || node.kind == tsp::Kind::Icon || node.kind == tsp::Kind::Rate) {
		layoutText(cache.text, engine, std::max(0.0f, available - cache.pad * 2));
	}
	return {available, cache.height};
}

bool paintInline(const tsp::Node& node, InlineCache& cache, text::Engine& engine, const text::Fonts& fonts,
                 SDL_Renderer& renderer, const SDL_FRect& rect, view::HitTargets& hits) {
	cache.nextWake = 0;
	gfx::ScopedClip clip(renderer, rect);
	if (clip.visible.w <= 0 || clip.visible.h <= 0)
		return false;
	measureInline(node, cache, engine, fonts, rect.w);
	const Uint64 now = SDL_GetTicks();
	const float unit = fonts.size() / 15;
	const float top = rect.y + (rect.h - cache.height) * 0.5f;
	switch (node.kind) {
	case tsp::Kind::Badge: {
		auto color = cache.color;
		color.a = 45;
		const SDL_FRect pill{rect.x, top, cache.width, cache.height};
		gfx::rounded(renderer, pill, pill.h * 0.5f, color);
		drawText(cache.text, engine, rect.x + cache.pad, top + (cache.height - cache.text.layout.height) * 0.5f, hits);
		break;
	}
	case tsp::Kind::Kbd: {
		float x = rect.x, y = top + (cache.height - cache.text.layout.height) * 0.5f;
		if (cache.send) {
			const float side = std::min({cache.width, rect.h, cache.naturalWidth});
			const SDL_FRect button{rect.x + (rect.w - side) * 0.5f, rect.y + (rect.h - side) * 0.5f, side, side};
			gfx::rounded(renderer, button, side * 0.5f, text::palette::surface2);
			gfx::roundedBorder(renderer, button, side * 0.5f, text::palette::border, unit);
			x = button.x + (side - cache.keyWidth) * 0.5f;
			y = button.y + (side - cache.text.layout.height) * 0.5f;
		} else
			for (const auto& piece : cache.text.layout.pieces) {
				const SDL_FRect cap{rect.x + piece.x - cache.pad, top, piece.width + cache.pad * 2, cache.height};
				gfx::rounded(renderer, cap, unit * 3, text::palette::surface2);
				gfx::roundedBorder(renderer, cap, unit * 3, text::palette::border, unit);
			}
		engine.draw(cache.text.runs, cache.text.layout, x, y);
		break;
	}
	case tsp::Kind::Spinner: {
		const auto& frame = cache.glyphs[static_cast<size_t>((now / 83) % spinnerFrames.size())];
		engine.draw(frame.runs, frame.layout, rect.x + (cache.trackNaturalWidth - frame.naturalWidth) * 0.5f,
		            top + (cache.height - frame.layout.height) * 0.5f);
		drawText(cache.label, engine, rect.x + cache.labelOffset,
		         top + (cache.height - cache.label.layout.height) * 0.5f, hits);
		if (gfx::intersects({rect.x, top, cache.trackNaturalWidth, cache.height}, clip.visible))
			wake(cache, (now / 83 + 1) * 83);
		break;
	}
	case tsp::Kind::Elapsed: {
		float x = rect.x;
		for (const char c : cache.elapsedText) {
			if (c == '\0')
				break;
			const auto& part = cache.glyphs[elapsedAlphabet.find(c)];
			engine.draw(part.runs, part.layout, x, top + (cache.height - part.layout.height) * 0.5f);
			x += part.naturalWidth;
		}
		if (!cache.stopped)
			wake(cache, cache.textWake);
		break;
	}
	case tsp::Kind::Progress: {
		const SDL_FRect track{rect.x, top + (cache.height - cache.thickness) * 0.5f, cache.trackWidth, cache.thickness};
		gfx::rounded(renderer, track, track.h * 0.5f, text::palette::surface2);
		if (cache.known)
			gfx::rounded(renderer, {track.x, track.y, track.w * cache.value, track.h}, track.h * 0.5f, cache.color);
		else {
			const float phase = static_cast<float>(now % 1250) / 1250;
			const float left = track.w * (phase * 1.25f - 0.25f), right = left + track.w * 0.25f;
			gfx::rounded(renderer,
			             {track.x + std::max(0.0f, left), track.y,
			              std::max(0.0f, std::min(track.w, right) - std::max(0.0f, left)), track.h},
			             track.h * 0.5f, cache.color);
			if (gfx::intersects(track, clip.visible))
				wake(cache, (now / 33 + 1) * 33);
		}
		drawText(cache.label, engine, rect.x + cache.labelOffset,
		         top + (cache.height - cache.label.layout.height) * 0.5f, hits);
		break;
	}
	case tsp::Kind::Meter:
		paintMeter(cache, fonts, engine, renderer, rect, hits);
		break;
	case tsp::Kind::Effort:
		paintEffort(cache, fonts, renderer, rect, now);
		if (cache.rung == 6
		    && gfx::intersects(
		        {rect.x, rect.y + (rect.h - fonts.size() * 0.84f) * 0.5f, cache.naturalWidth, fonts.size() * 0.84f},
		        clip.visible))
			wake(cache, (now / 33 + 1) * 33);
		break;
	case tsp::Kind::Icon:
	case tsp::Kind::Rate:
		drawText(cache.text, engine, rect.x, top + (cache.height - cache.text.layout.height) * 0.5f, hits);
		break;
	default:
		break;
	}
	if (cache.textAnimated
	    && (animatedTextVisible(cache.text, rect.x + cache.pad, top + (cache.height - cache.text.layout.height) * 0.5f,
	                            clip.visible)
	        || animatedTextVisible(cache.label, rect.x + cache.labelOffset,
	                               top + (cache.height - cache.label.layout.height) * 0.5f, clip.visible)
	        || animatedTextVisible(cache.total, rect.x + cache.totalOffset,
	                               top + (cache.height - cache.total.layout.height) * 0.5f, clip.visible))) {
		wake(cache, (now / 33 + 1) * 33);
	}
	return cache.nextWake != 0;
}

} // namespace kinds
