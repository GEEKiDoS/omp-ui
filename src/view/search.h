#pragma once

#include "text/inline_layout.h"

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace view {

struct Match {
	std::uintptr_t owner = 0;
	std::uint32_t begin = 0, end = 0;
	float y = 0, height = 0;
};

class Search {
public:
	bool open = false, focused = false, caseSensitive = false, regex = false, invalid = false, capped = false;
	std::string query;
	std::vector<Match> matches;
	size_t current = 0;
	Uint64 due = 0;
	double milliseconds = 0;
	int navigation = 0;
	// Set before ordinary painting, so repeated draws of shared Runs have distinct identities.
	float scroll = 0;

	void show(std::string_view selected);
	void close();
	void setQuery(std::string_view value);
	void setOption(std::string_view option, bool enabled);
	bool key(SDL_Keycode key, SDL_Keymod mod, std::string_view typed);
	void preedit(std::string_view value, int start, int length);
	bool pointer(const SDL_Event& event, float sx, float sy);
	void paint(SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts, SDL_FRect area);
	std::optional<SDL_FRect> caret() const;

	void begin();
	void collect(const text::Runs& runs, const text::Layout& layout, float x, float y, float scrollOffset,
	             SDL_FRect clip);
	void finish();
	void highlight(text::Engine& engine, const text::Runs& runs, const text::Layout& layout, float x, float y,
	               float opacity);
	const std::string& statusText() const;

private:
	enum class Control { None, Panel, Field, Case, Regex, Previous, Next, Close };
	struct CachedText {
		text::Runs runs;
		text::Layout layout;
		float em = 0, width = 0;
	};
	struct Fragment {
		size_t begin, end;
		float y, height;
	};
	struct OwnerMatch {
		std::uintptr_t owner;
		std::uint32_t index;
	};

	void changed();
	void replace(std::string_view value);
	void move(size_t byte, bool selecting);
	void clearPreedit();
	void updateField(text::Engine& engine, const text::Fonts& fonts);
	static void updateLabel(CachedText& cache, text::Engine& engine, const text::Fonts& fonts, std::string_view value,
	                        SDL_Color color, float width);
	Control controlAt(float x, float y) const;
	size_t byteAt(float x, float y);
	bool searchSegment(std::uintptr_t owner);
	bool addMatch(std::uintptr_t owner, size_t begin, size_t end);

	size_t cursor_ = 0, anchor_ = 0;
	std::string composition_;
	int compositionStart_ = 0, compositionLength_ = 0;
	bool fieldDirty_ = true, dragging_ = false;
	Uint32 pressedButtons_ = 0;
	Control pressed_ = Control::None, hovered_ = Control::None;
	float textScroll_ = 0, textX_ = 0, textY_ = 0;
	float cursorX_ = 0, selectionFrom_ = 0, selectionTo_ = 0;
	size_t metricsCursor_ = std::string::npos, metricsAnchor_ = std::string::npos;
	SDL_FRect panel_{}, field_{}, fieldClip_{}, countRect_{}, caseRect_{}, regexRect_{}, previousRect_{}, nextRect_{},
	    closeRect_{};
	std::optional<SDL_FRect> caret_;
	std::optional<std::reference_wrapper<text::Engine>> engine_;
	CachedText fieldText_, placeholder_, countText_, caseText_, regexText_;
	mutable std::string shownText_;
	mutable size_t shownCurrent_ = 0, shownCount_ = 0;
	mutable int shownStatus_ = -1;
	mutable bool shownCapped_ = false;

	std::optional<std::regex> expression_;
	std::string needle_, segment_;
	std::vector<size_t> runBases_;
	std::vector<Fragment> fragments_;
	std::vector<OwnerMatch> owners_;
	size_t segmentBegin_ = 0;
};

} // namespace view
