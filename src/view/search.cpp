#include "search.h"

#include "core/text_util.h"
#include "gfx/draw.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

namespace view {
namespace {

constexpr size_t matchLimit = 10000;
constexpr float textWidthLimit = std::numeric_limits<float>::max() / 4;

bool contains(const SDL_FRect& rect, float x, float y) {
	return rect.w > 0 && rect.h > 0 && x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

size_t boundaryBefore(std::string_view value, size_t byte) {
	byte = std::min(byte, value.size());
	while (byte > 0 && byte < value.size() && (static_cast<unsigned char>(value[byte]) & 0xc0) == 0x80)
		--byte;
	return byte;
}

size_t previousByte(std::string_view value, size_t byte) {
	return byte == 0 ? 0 : boundaryBefore(value, byte - 1);
}

size_t nextByte(std::string_view value, size_t byte) {
	if (byte == value.size())
		return byte;
	++byte;
	while (byte < value.size() && (static_cast<unsigned char>(value[byte]) & 0xc0) == 0x80)
		++byte;
	return byte;
}

size_t characterByte(std::string_view value, int characters) {
	size_t byte = 0;
	for (int i = 0; i < characters && byte < value.size(); ++i)
		byte = nextByte(value, byte);
	return byte;
}

std::string singleLine(std::string_view value) {
	std::string result;
	result.reserve(value.size());
	for (size_t byte = 0; byte < value.size();) {
		const auto cp = text::decode(value, byte);
		if (cp == '\r' && byte < value.size() && value[byte] == '\n')
			++byte;
		if (cp == '\r' || cp == '\n' || cp == '\t' || cp == 0x85 || cp == 0x2028 || cp == 0x2029)
			result.push_back(' ');
		else if (cp >= 0x20 && !(cp >= 0x7f && cp < 0xa0))
			text::append(result, cp);
	}
	return result;
}

char folded(char value) {
	return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

std::uint64_t mixed(std::uint64_t value) {
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

std::uintptr_t ownerAt(const text::Runs& runs, float x, float y) {
	// Some renderers reuse scratch Runs at several positions in one paint.
	const auto address = reinterpret_cast<std::uintptr_t>(&runs);
	const auto ix = static_cast<std::uint64_t>(std::llround(x));
	const auto iy = static_cast<std::uint64_t>(std::llround(y));
	return static_cast<std::uintptr_t>(mixed(address) ^ mixed(ix + 0x9e3779b97f4a7c15ULL)
	                                   ^ mixed(iy + 0x3c6ef372fe94f82aULL));
}

float byteX(const text::Runs& runs, size_t byte, const text::Fonts& fonts) {
	float x = 0;
	for (const auto& run : runs) {
		const auto count = std::min(byte, run.text.size());
		x += fonts.width(text::FontId::Ui, std::string_view(run.text).substr(0, count));
		byte -= count;
		if (byte == 0)
			break;
	}
	return x;
}

} // namespace

void Search::changed() {
	matches.clear();
	owners_.clear();
	current = 0;
	navigation = 0;
	invalid = capped = false;
	milliseconds = 0;
	due = SDL_GetTicks() + 120;
}

void Search::show(std::string_view selected) {
	if (!selected.empty())
		setQuery(selected);
	open = focused = true;
	clearPreedit();
	anchor_ = 0;
	cursor_ = query.size();
}

void Search::close() {
	open = focused = false;
	// A changed query may still be pending when reopened; a closed field never wakes.
	navigation = 0;
	dragging_ = false;
	pressed_ = hovered_ = Control::None;
	caret_.reset();
	clearPreedit();
}

void Search::setQuery(std::string_view value) {
	auto cleaned = singleLine(value);
	if (query != cleaned) {
		query = std::move(cleaned);
		fieldDirty_ = true;
		changed();
	}
	cursor_ = anchor_ = query.size();
	clearPreedit();
}

void Search::setOption(std::string_view option, bool enabled) {
	if (option == "case" && caseSensitive != enabled) {
		caseSensitive = enabled;
		changed();
	} else if (option == "regex" && regex != enabled) {
		regex = enabled;
		changed();
	}
}

void Search::clearPreedit() {
	if (!composition_.empty()) {
		composition_.clear();
		fieldDirty_ = true;
	}
	compositionStart_ = compositionLength_ = 0;
}

void Search::move(size_t byte, bool selecting) {
	cursor_ = boundaryBefore(query, byte);
	if (!selecting)
		anchor_ = cursor_;
	clearPreedit();
}

void Search::replace(std::string_view value) {
	const auto cleaned = singleLine(value);
	const auto first = std::min(cursor_, anchor_), last = std::max(cursor_, anchor_);
	if (first != last || !cleaned.empty()) {
		query.replace(first, last - first, cleaned);
		cursor_ = anchor_ = first + cleaned.size();
		fieldDirty_ = true;
		changed();
	}
	clearPreedit();
}

bool Search::key(SDL_Keycode key, SDL_Keymod mod, std::string_view typed) {
	if (!open)
		return false;
	const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
	const bool command = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
	const bool alt = (mod & SDL_KMOD_ALT) != 0;
	// F3 steps through matches wherever focus is; everything else belongs to the field only while
	// it is focused, so Enter, Esc and Alt+… reach omp's prompt otherwise.
	if (key == SDLK_F3 || (focused && (key == SDLK_RETURN || key == SDLK_KP_ENTER))) {
		navigation = shift ? -1 : 1;
		return true;
	}
	if (!focused)
		return false;
	if (key == SDLK_ESCAPE) {
		close();
		return true;
	}
	if (alt && !command && (key == SDLK_C || key == SDLK_R)) {
		if (key == SDLK_C)
			setOption("case", !caseSensitive);
		else
			setOption("regex", !regex);
		return true;
	}
	if ((command && !alt && key == SDLK_V) || (shift && !command && !alt && key == SDLK_INSERT)) {
		std::unique_ptr<char, decltype(&SDL_free)> clipboard(SDL_GetClipboardText(), SDL_free);
		if (clipboard)
			replace(clipboard.get());
		return true;
	}
	if (command && !alt) {
		if (key == SDLK_A) {
			anchor_ = 0;
			cursor_ = query.size();
			clearPreedit();
			return true;
		}
		if (key == SDLK_C || key == SDLK_X) {
			const auto first = std::min(cursor_, anchor_), count = std::max(cursor_, anchor_) - first;
			if (count != 0) {
				const auto selected = query.substr(first, count);
				SDL_SetClipboardText(selected.c_str());
				if (key == SDLK_X)
					replace({});
			}
			return true;
		}
	}
	if (!typed.empty() && ((!command && !alt) || (command && alt))) {
		replace(typed);
		return true;
	}
	if (!alt) {
		switch (key) {
		case SDLK_LEFT:
			move(!shift && cursor_ != anchor_ ? std::min(cursor_, anchor_) : previousByte(query, cursor_), shift);
			return true;
		case SDLK_RIGHT:
			move(!shift && cursor_ != anchor_ ? std::max(cursor_, anchor_) : nextByte(query, cursor_), shift);
			return true;
		case SDLK_HOME:
			move(0, shift);
			return true;
		case SDLK_END:
			move(query.size(), shift);
			return true;
		case SDLK_BACKSPACE:
			if (cursor_ == anchor_)
				anchor_ = previousByte(query, cursor_);
			replace({});
			return true;
		case SDLK_DELETE:
			if (cursor_ == anchor_)
				anchor_ = nextByte(query, cursor_);
			replace({});
			return true;
		case SDLK_TAB:
			return true;
		default:
			break;
		}
	}
	// Focused search owns keyboard input, including keys without an editing action.
	return true;
}

void Search::preedit(std::string_view value, int start, int length) {
	if (!open || !focused)
		return;
	auto cleaned = singleLine(value);
	start = std::max(0, start);
	length = std::max(0, length);
	if (composition_ != cleaned || compositionStart_ != start || compositionLength_ != length) {
		composition_ = std::move(cleaned);
		compositionStart_ = start;
		compositionLength_ = length;
		fieldDirty_ = true;
	}
}

Search::Control Search::controlAt(float x, float y) const {
	if (!contains(panel_, x, y))
		return Control::None;
	if (contains(field_, x, y))
		return Control::Field;
	if (contains(caseRect_, x, y))
		return Control::Case;
	if (contains(regexRect_, x, y))
		return Control::Regex;
	if (contains(previousRect_, x, y))
		return Control::Previous;
	if (contains(nextRect_, x, y))
		return Control::Next;
	if (contains(closeRect_, x, y))
		return Control::Close;
	return Control::Panel;
}

size_t Search::byteAt(float x, float y) {
	if (!engine_)
		return cursor_;
	auto byte = engine_->get().hit(fieldText_.runs, fieldText_.layout, x - textX_, y - textY_).byte;
	if (!composition_.empty()) {
		const auto first = std::min(cursor_, anchor_), last = std::max(cursor_, anchor_);
		if (byte > first)
			byte = byte < first + composition_.size() ? first : last + byte - first - composition_.size();
	}
	return boundaryBefore(query, byte);
}

bool Search::pointer(const SDL_Event& event, float sx, float sy) {
	// A close or Escape between press and release must not leak the release to the transcript.
	if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
		const auto mask = SDL_BUTTON_MASK(event.button.button);
		if ((pressedButtons_ & mask) != 0) {
			pressedButtons_ &= ~mask;
			if (event.button.button == SDL_BUTTON_LEFT) {
				const auto target = open ? controlAt(event.button.x * sx, event.button.y * sy) : Control::None;
				const auto pressed = std::exchange(pressed_, Control::None);
				dragging_ = false;
				if (target == pressed) {
					switch (target) {
					case Control::Case:
						setOption("case", !caseSensitive);
						break;
					case Control::Regex:
						setOption("regex", !regex);
						break;
					case Control::Previous:
						navigation = -1;
						break;
					case Control::Next:
						navigation = 1;
						break;
					case Control::Close:
						close();
						break;
					default:
						break;
					}
				}
			}
			return true;
		}
	}
	if (!open)
		return false;
	switch (event.type) {
	case SDL_EVENT_MOUSE_MOTION: {
		const float x = event.motion.x * sx, y = event.motion.y * sy;
		hovered_ = controlAt(x, y);
		if (dragging_) {
			move(byteAt(x, y), true);
			return true;
		}
		return hovered_ != Control::None;
	}
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		const float x = event.button.x * sx, y = event.button.y * sy;
		const auto target = controlAt(x, y);
		if (target == Control::None) {
			focused = false;
			clearPreedit();
			caret_.reset();
			return false;
		}
		pressedButtons_ |= SDL_BUTTON_MASK(event.button.button);
		if (event.button.button == SDL_BUTTON_LEFT) {
			pressed_ = target;
			if (target == Control::Field) {
				focused = true;
				const auto byte = byteAt(x, y);
				move(byte, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
				if (event.button.clicks >= 2) {
					anchor_ = 0;
					cursor_ = query.size();
				}
				dragging_ = true;
			}
		}
		return true;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP:
		return controlAt(event.button.x * sx, event.button.y * sy) != Control::None;
	case SDL_EVENT_MOUSE_WHEEL:
		return controlAt(event.wheel.mouse_x * sx, event.wheel.mouse_y * sy) != Control::None;
	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		hovered_ = Control::None;
		return false;
	default:
		return false;
	}
}

void Search::updateLabel(CachedText& cache, text::Engine& engine, const text::Fonts& fonts, std::string_view value,
                         SDL_Color color, float width) {
	const bool changed =
	    cache.runs.empty() || cache.runs.front().text != value || cache.em != fonts.size() || cache.width != width;
	if (cache.runs.empty())
		cache.runs.push_back({std::string(value), {.color = color}});
	else {
		if (cache.runs.front().text != value)
			cache.runs.front().text = value;
		cache.runs.front().style.color = color;
	}
	if (changed) {
		cache.layout =
		    engine.layout(cache.runs, {.width = std::max(1.0f, width), .wrap = text::Wrap::None, .maxLines = 1});
		cache.em = fonts.size();
		cache.width = width;
	}
}

void Search::updateField(text::Engine& engine, const text::Fonts& fonts) {
	if (!fieldDirty_ && fieldText_.em == fonts.size()) {
		for (auto& run : fieldText_.runs)
			run.style.color = text::palette::fg;
		if (!composition_.empty())
			fieldText_.runs[2].style.bg = text::withAlpha(text::palette::selected, 150);
		return;
	}
	fieldText_.runs.clear();
	text::RunStyle style{.color = text::palette::fg};
	if (composition_.empty())
		fieldText_.runs.push_back({query, style});
	else {
		const auto first = std::min(cursor_, anchor_), last = std::max(cursor_, anchor_);
		const auto begin = characterByte(composition_, compositionStart_);
		const auto end = begin + characterByte(std::string_view(composition_).substr(begin), compositionLength_);
		fieldText_.runs.push_back({query.substr(0, first), style});
		style.underline = true;
		fieldText_.runs.push_back({composition_.substr(0, begin), style});
		style.bg = text::withAlpha(text::palette::selected, 150);
		fieldText_.runs.push_back({composition_.substr(begin, end - begin), style});
		style.bg = {0, 0, 0, 0};
		fieldText_.runs.push_back({composition_.substr(end), style});
		style.underline = false;
		fieldText_.runs.push_back({query.substr(last), style});
	}
	fieldText_.layout =
	    engine.layout(fieldText_.runs, {.width = textWidthLimit, .wrap = text::Wrap::None, .maxLines = 1});
	fieldText_.em = fonts.size();
	metricsCursor_ = std::string::npos;
	fieldDirty_ = false;
}

void Search::paint(SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts, SDL_FRect area) {
	caret_.reset();
	if (!open || area.w <= 0 || area.h <= 0)
		return;
	engine_ = engine;
	const float em = fonts.size(), unit = std::max(1.0f, em / 15), gap = em * 0.2f, pad = em * 0.4f;
	const float margin = std::min(em * 0.5f, area.w * 0.025f);
	const float width = std::min(em * 35, std::max(0.0f, area.w - margin * 2));
	const float row = std::max(fonts.lineHeight() + em * 0.35f, em * 1.65f);
	const float button = row, countWidth = em * 8.6f;
	const bool compact = width < em * 8 + countWidth + button * 5 + gap * 6 + pad * 2;
	panel_ = {area.x + area.w - margin - width, area.y + margin, width,
	          row * (compact ? 2 : 1) + pad * 2 + (compact ? gap : 0)};
	const float left = panel_.x + pad, top = panel_.y + pad, inner = std::max(0.0f, width - pad * 2);
	if (!compact) {
		float right = left + inner;
		const auto slot = [&]() {
			right -= button;
			const SDL_FRect result{right, top, button, row};
			right -= gap;
			return result;
		};
		closeRect_ = slot();
		nextRect_ = slot();
		previousRect_ = slot();
		regexRect_ = slot();
		caseRect_ = slot();
		countRect_ = {right - countWidth, top, countWidth, row};
		field_ = {left, top, std::max(0.0f, countRect_.x - gap - left), row};
	} else {
		const float closeWidth = std::min(button, inner * 0.2f);
		closeRect_ = {left + inner - closeWidth, top, closeWidth, row};
		field_ = {left, top, std::max(0.0f, inner - closeWidth - gap), row};
		const float second = top + row + gap;
		const float smallButton = std::min(button, std::max(0.0f, (inner - gap * 4) / 6));
		float right = left + inner;
		const auto slot = [&]() {
			right -= smallButton;
			const SDL_FRect result{right, second, smallButton, row};
			right -= gap;
			return result;
		};
		nextRect_ = slot();
		previousRect_ = slot();
		regexRect_ = slot();
		caseRect_ = slot();
		countRect_ = {left, second, std::max(0.0f, right - left), row};
	}
	gfx::ScopedClip panelClip(renderer, area);
	gfx::rounded(renderer, panel_, unit * 4, text::palette::surface);
	gfx::roundedBorder(renderer, panel_, unit * 4, text::palette::border, unit);
	gfx::rounded(renderer, field_, unit * 3, text::palette::bg);
	if (invalid)
		gfx::rounded(renderer, field_, unit * 3, text::withAlpha(text::palette::error, 35));
	gfx::roundedBorder(renderer, field_, unit * 3,
	                   invalid   ? text::palette::error
	                   : focused ? text::palette::focus
	                             : text::palette::border,
	                   unit);
	fieldClip_ = {field_.x + pad, field_.y + unit, std::max(0.0f, field_.w - pad * 2),
	              std::max(0.0f, field_.h - unit * 2)};
	updateField(engine, fonts);
	updateLabel(countText_, engine, fonts, statusText(), invalid ? text::palette::error : text::palette::muted,
	            countRect_.w);
	updateLabel(caseText_, engine, fonts, "Aa", caseSensitive ? text::palette::accent : text::palette::muted,
	            caseRect_.w);
	updateLabel(regexText_, engine, fonts, ".*", regex ? text::palette::accent : text::palette::muted, regexRect_.w);
	updateLabel(placeholder_, engine, fonts, "Find in transcript", text::palette::dim, fieldClip_.w);
	const auto label = [&](const CachedText& cache, const SDL_FRect& rect) {
		gfx::ScopedClip clip(renderer, rect);
		engine.draw(cache.runs, cache.layout, rect.x + std::max(0.0f, (rect.w - cache.layout.width) * 0.5f),
		            rect.y + (rect.h - cache.layout.height) * 0.5f);
	};
	label(countText_, countRect_);
	const auto buttonBack = [&](Control control, const SDL_FRect& rect, bool on) {
		if (on || hovered_ == control || pressed_ == control)
			gfx::rounded(renderer, rect, unit * 3,
			             on                    ? text::palette::selected
			             : pressed_ == control ? text::palette::surface2
			                                   : text::palette::hover);
		if (on)
			gfx::roundedBorder(renderer, rect, unit * 3, text::palette::accent, unit);
	};
	buttonBack(Control::Case, caseRect_, caseSensitive);
	buttonBack(Control::Regex, regexRect_, regex);
	buttonBack(Control::Previous, previousRect_, false);
	buttonBack(Control::Next, nextRect_, false);
	buttonBack(Control::Close, closeRect_, false);
	label(caseText_, caseRect_);
	label(regexText_, regexRect_);
	const auto chevron = [&](const SDL_FRect& rect, bool down) {
		const float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
		const float radius = std::min(em * 0.23f, rect.w * 0.25f), sign = down ? 1.0f : -1.0f;
		const auto color = matches.empty() || invalid ? text::palette::dim : text::palette::fg;
		gfx::line(renderer, {cx - radius, cy - radius * sign * 0.5f}, {cx, cy + radius * sign * 0.5f}, color,
		          unit * 1.4f);
		gfx::line(renderer, {cx, cy + radius * sign * 0.5f}, {cx + radius, cy - radius * sign * 0.5f}, color,
		          unit * 1.4f);
	};
	chevron(previousRect_, false);
	chevron(nextRect_, true);
	const float cx = closeRect_.x + closeRect_.w * 0.5f, cy = closeRect_.y + closeRect_.h * 0.5f;
	const float radius = std::min(em * 0.23f, closeRect_.w * 0.25f);
	gfx::line(renderer, {cx - radius, cy - radius}, {cx + radius, cy + radius}, text::palette::muted, unit * 1.4f);
	gfx::line(renderer, {cx - radius, cy + radius}, {cx + radius, cy - radius}, text::palette::muted, unit * 1.4f);

	if (metricsCursor_ != cursor_ || metricsAnchor_ != anchor_) {
		metricsCursor_ = cursor_;
		metricsAnchor_ = anchor_;
		const size_t displayCaret = composition_.empty()
		                              ? cursor_
		                              : std::min(cursor_, anchor_) + characterByte(composition_, compositionStart_);
		cursorX_ = byteX(fieldText_.runs, displayCaret, fonts);
		if (composition_.empty() && cursor_ != anchor_) {
			selectionFrom_ = cursor_ < anchor_ ? cursorX_ : byteX(fieldText_.runs, anchor_, fonts);
			selectionTo_ = cursor_ > anchor_ ? cursorX_ : byteX(fieldText_.runs, anchor_, fonts);
		}
	}
	if (cursorX_ < textScroll_)
		textScroll_ = cursorX_;
	else if (cursorX_ > textScroll_ + fieldClip_.w - unit * 2)
		textScroll_ = cursorX_ - std::max(0.0f, fieldClip_.w - unit * 2);
	textScroll_ = std::clamp(textScroll_, 0.0f, std::max(0.0f, fieldText_.layout.width - fieldClip_.w + unit * 2));
	textX_ = fieldClip_.x - textScroll_;
	textY_ = field_.y + (field_.h - fieldText_.layout.height) * 0.5f;
	if (fieldClip_.w <= 0 || fieldClip_.h <= 0)
		return;
	gfx::ScopedClip fieldClip(renderer, fieldClip_);
	if (focused && composition_.empty() && cursor_ != anchor_) {
		gfx::fill(renderer,
		          {textX_ + selectionFrom_, textY_, selectionTo_ - selectionFrom_, fieldText_.layout.lineHeight},
		          text::withAlpha(text::palette::accent, 85));
	}
	if (query.empty() && composition_.empty())
		engine.draw(placeholder_.runs, placeholder_.layout, fieldClip_.x,
		            field_.y + (field_.h - placeholder_.layout.height) * 0.5f);
	else
		engine.draw(fieldText_.runs, fieldText_.layout, textX_, textY_);
	if (focused) {
		const float height = std::min(fonts.lineHeight(), fieldClip_.h);
		const SDL_FRect cursor{textX_ + cursorX_, field_.y + (field_.h - height) * 0.5f, unit * 1.5f, height};
		caret_ = gfx::intersect(cursor, fieldClip_);
		gfx::fill(renderer, cursor, text::palette::accent);
	}
}

std::optional<SDL_FRect> Search::caret() const {
	return open && focused ? caret_ : std::nullopt;
}

void Search::begin() {
	matches.clear();
	owners_.clear();
	invalid = capped = false;
	milliseconds = 0;
	expression_.reset();
	if (query.empty())
		return;
	if (regex) {
		auto flags = std::regex_constants::ECMAScript;
		if (!caseSensitive)
			flags |= std::regex_constants::icase;
		try {
			expression_.emplace(query, flags);
		} catch (const std::regex_error&) {
			invalid = true;
		}
	} else {
		needle_ = query;
		if (!caseSensitive)
			for (auto& byte : needle_)
				byte = folded(byte);
	}
}

bool Search::addMatch(std::uintptr_t owner, size_t begin, size_t end) {
	if (matches.size() == matchLimit) {
		capped = true;
		return false;
	}
	const size_t globalBegin = segmentBegin_ + begin, globalEnd = segmentBegin_ + end;
	const auto first = std::lower_bound(fragments_.begin(), fragments_.end(), globalBegin,
	                                    [](const Fragment& fragment, size_t byte) { return fragment.end <= byte; });
	const auto start = first == fragments_.end() ? std::prev(fragments_.end()) : first;
	float top = start->y, bottom = top + start->height;
	for (auto fragment = start; fragment != fragments_.end() && fragment->begin < globalEnd; ++fragment) {
		top = std::min(top, fragment->y);
		bottom = std::max(bottom, fragment->y + fragment->height);
	}
	matches.push_back(
	    {owner, static_cast<std::uint32_t>(globalBegin), static_cast<std::uint32_t>(globalEnd), top, bottom - top});
	return true;
}

bool Search::searchSegment(std::uintptr_t owner) {
	if (segment_.empty())
		return true;
	if (regex) {
		try {
			for (auto match = std::sregex_iterator(segment_.begin(), segment_.end(), *expression_);
			     match != std::sregex_iterator(); ++match) {
				const auto begin = static_cast<size_t>(match->position()),
				           end = begin + static_cast<size_t>(match->length());
				if (!addMatch(owner, begin, end))
					return false;
			}
		} catch (const std::regex_error&) {
			// std::regex may also report complexity/stack limits while matching user input.
			invalid = true;
			matches.clear();
			return false;
		}
	} else {
		if (!caseSensitive)
			for (auto& byte : segment_)
				byte = folded(byte);
		for (size_t begin = segment_.find(needle_); begin != std::string::npos;
		     begin = segment_.find(needle_, begin + needle_.size())) {
			if (!addMatch(owner, begin, begin + needle_.size()))
				return false;
		}
	}
	return true;
}

void Search::collect(const text::Runs& runs, const text::Layout& layout, float x, float y, float scrollOffset,
                     SDL_FRect clip) {
	if (query.empty() || invalid || capped)
		return;
	runBases_.clear();
	runBases_.reserve(runs.size() + 1);
	runBases_.push_back(0);
	for (const auto& run : runs)
		runBases_.push_back(runBases_.back() + run.text.size());
	segment_.clear();
	fragments_.clear();
	const auto owner = ownerAt(runs, x, y + scrollOffset);
	const float lineHeight = layout.lineHeight;
	for (const auto& piece : layout.pieces) {
		if (piece.begin == piece.end && !piece.ellipsis)
			continue;
		const auto& run = runs[piece.run];
		const float top = y + static_cast<float>(piece.line) * lineHeight;
		const SDL_FRect rect{x + piece.x, top, std::max(1.0f, piece.width), lineHeight};
		const bool visible = !piece.ellipsis && !run.style.icon && run.style.font != text::FontId::Icons
		                  && !run.style.chrome && gfx::intersects(rect, clip);
		const auto begin = runBases_[piece.run] + piece.begin, end = runBases_[piece.run] + piece.end;
		if (!visible || (!segment_.empty() && begin != segmentBegin_ + segment_.size())) {
			if (!searchSegment(owner))
				return;
			segment_.clear();
			fragments_.clear();
		}
		if (!visible)
			continue;
		if (segment_.empty())
			segmentBegin_ = begin;
		segment_.append(run.text, piece.begin, piece.end - piece.begin);
		fragments_.push_back({begin, end, top + scrollOffset, lineHeight});
	}
	searchSegment(owner);
}

void Search::finish() {
	std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.y < b.y; });
	current = matches.empty() ? 0 : std::min(current, matches.size() - 1);
	owners_.clear();
	owners_.reserve(matches.size());
	for (size_t index = 0; index < matches.size(); ++index)
		owners_.push_back({matches[index].owner, static_cast<std::uint32_t>(index)});
	std::sort(owners_.begin(), owners_.end(), [](const OwnerMatch& a, const OwnerMatch& b) {
		return a.owner < b.owner || (a.owner == b.owner && a.index < b.index);
	});
	due = 0;
}

void Search::highlight(text::Engine& engine, const text::Runs& runs, const text::Layout& layout, float x, float y,
                       float opacity) {
	if (!open || invalid || matches.empty())
		return;
	const auto owner = ownerAt(runs, x, y + scroll);
	auto found = std::lower_bound(owners_.begin(), owners_.end(), owner,
	                              [](const OwnerMatch& entry, std::uintptr_t value) { return entry.owner < value; });
	for (; found != owners_.end() && found->owner == owner; ++found) {
		const auto& match = matches[found->index];
		engine.paintHighlight(runs, layout, x, y, match.begin, match.end, found->index == current, opacity);
	}
}

const std::string& Search::statusText() const {
	const int state = invalid ? 0 : query.empty() ? 1 : due != 0 ? 2 : matches.empty() ? 3 : 4;
	if (state == shownStatus_ && current == shownCurrent_ && matches.size() == shownCount_ && capped == shownCapped_)
		return shownText_;
	shownStatus_ = state;
	shownCurrent_ = current;
	shownCount_ = matches.size();
	shownCapped_ = capped;
	switch (state) {
	case 0:
		shownText_ = "Invalid regex";
		break;
	case 1:
		shownText_ = "No query";
		break;
	case 2:
		shownText_ = "Searching...";
		break;
	case 3:
		shownText_ = "No results";
		break;
	default:
		shownText_ = std::to_string(std::min(current, matches.size() - 1) + 1) + " of " + std::to_string(matches.size())
		           + (capped ? "+" : "");
		break;
	}
	return shownText_;
}

} // namespace view
