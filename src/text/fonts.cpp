#include "fonts.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef OMP_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

namespace text {
namespace {

#ifdef OMP_FONTCONFIG
struct ConfigDeleter {
	void operator()(FcConfig* value) const { FcConfigDestroy(value); }
};
struct PatternDeleter {
	void operator()(FcPattern* value) const { FcPatternDestroy(value); }
};
std::string systemFont(FcConfig& config, const char* name) {
	std::unique_ptr<FcPattern, PatternDeleter> pattern(FcNameParse(reinterpret_cast<const FcChar8*>(name)));
	FcConfigSubstitute(&config, pattern.get(), FcMatchPattern);
	FcDefaultSubstitute(pattern.get());
	FcResult result;
	std::unique_ptr<FcPattern, PatternDeleter> match(FcFontMatch(&config, pattern.get(), &result));
	FcChar8* file = nullptr;
	if (match && FcPatternGetString(match.get(), FC_FILE, 0, &file) == FcResultMatch)
		return reinterpret_cast<const char*>(file);
	return {};
}
#endif

} // namespace

bool Fonts::load(float displayScale, std::string& error) {
	size_ = 15 * displayScale;
	// Bundled faces live in `fonts/` beside the executable (UTF-8 paths, as SDL_ttf takes them).
	const std::string base = std::string(SDL_GetBasePath()) + "fonts/";
	auto fallback = [&](const std::filesystem::path& path) {
		if (!std::filesystem::exists(path))
			return;
		Font face(TTF_OpenFont(path.string().c_str(), size_));
		if (face)
			fallback_.push_back(std::move(face));
	};
	Font nerd(TTF_OpenFont((base + "SymbolsNerdFontMono-Regular.ttf").c_str(), size_));
	if (!nerd) {
		error = SDL_GetError();
		return false;
	}
	fallback_.push_back(std::move(nerd));
#ifdef _WIN32
	std::array<wchar_t, MAX_PATH> windows{};
	const auto length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
	if (length == 0 || length >= windows.size()) {
		error = "Cannot locate the Windows fonts directory";
		return false;
	}
	const std::filesystem::path system = std::filesystem::path(windows.data()) / "Fonts";
	fallback(system / "seguiemj.ttf");
	fallback(system / "msyh.ttc");
	fallback(system / "seguisym.ttf");
#elif defined(__APPLE__)
	fallback("/System/Library/Fonts/Apple Color Emoji.ttc");
	fallback("/System/Library/Fonts/PingFang.ttc");
	fallback("/System/Library/Fonts/Apple Symbols.ttf");
#elif defined(OMP_FONTCONFIG)
	std::unique_ptr<FcConfig, ConfigDeleter> config(FcInitLoadConfigAndFonts());
	if (config) {
		for (const auto name : {"emoji", "sans:lang=zh", "DejaVu Sans"}) {
			const auto path = systemFont(*config, name);
			if (!path.empty())
				fallback(path);
		}
	}
#endif
	constexpr std::array<std::string_view, 8> names = {
	    "Inter-Regular.ttf",         "Inter-Bold.ttf",         "Inter-Italic.ttf",          "Inter-Bold.ttf",
	    "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf", "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf"};
	for (size_t i = 0; i < names.size(); ++i) {
		fonts_[i].reset(TTF_OpenFont((base + std::string(names[i])).c_str(), size_));
		if (!fonts_[i]) {
			error = SDL_GetError();
			return false;
		}
		if (i == 3 || i == 6 || i == 7)
			TTF_SetFontStyle(fonts_[i].get(), TTF_STYLE_ITALIC);
		for (const auto& face : fallback_)
			TTF_AddFallbackFont(fonts_[i].get(), face.get());
	}
	auto& wordmark = fonts_[static_cast<size_t>(FontId::Wordmark)];
	wordmark.reset(TTF_OpenFont((base + "Inter-Bold.ttf").c_str(), size_ * 2));
	if (!wordmark) {
		error = SDL_GetError();
		return false;
	}
	for (const auto& face : fallback_)
		TTF_AddFallbackFont(wordmark.get(), face.get());
	measureRises();
	return true;
}

void Fonts::measureRises() {
	// SDL_ttf lowers a whole string by however far a glyph reaches above the face's height box;
	// an emoji is the tallest glyph text normally carries.
	constexpr std::string_view emoji = "\xF0\x9F\x98\x80";
	for (size_t i = 0; i < fonts_.size(); ++i) {
		int width = 0, height = 0;
		rises_[i] = fonts_[i] && TTF_GetStringSize(fonts_[i].get(), emoji.data(), emoji.size(), &width, &height)
		              ? std::max(0.0f, static_cast<float>(height - TTF_GetFontHeight(fonts_[i].get())))
		              : 0;
	}
}

void Fonts::setScale(float displayScale) {
	size_ = 15 * displayScale;
	for (const auto& face : fallback_)
		TTF_SetFontSize(face.get(), size_);
	for (size_t i = 0; i < fonts_.size(); ++i)
		if (fonts_[i])
			TTF_SetFontSize(fonts_[i].get(), size_ * (i == static_cast<size_t>(FontId::Wordmark) ? 2 : 1));
	for (auto& cache : widths_)
		cache.clear();
	measureRises();
}

float Fonts::width(FontId id, std::string_view value) const {
	if (value.empty())
		return 0;
	constexpr size_t memoLength = 48, memoEntries = 32768;
	auto& cache = widths_[static_cast<size_t>(id)];
	if (value.size() <= memoLength)
		if (const auto found = cache.find(value); found != cache.end())
			return found->second;
	int width = 0, height = 0;
	TTF_GetStringSize(&get(id), value.data(), value.size(), &width, &height);
	if (value.size() <= memoLength) {
		if (cache.size() >= memoEntries)
			cache.clear();
		cache.emplace(value, static_cast<float>(width));
	}
	return static_cast<float>(width);
}

size_t Fonts::fit(FontId id, std::string_view value, float width) const {
	if (value.empty() || width <= 0)
		return 0;
	int measured = 0;
	size_t length = 0;
	TTF_MeasureString(&get(id), value.data(), value.size(), static_cast<int>(std::floor(width)), &measured, &length);
	return length;
}

} // namespace text
