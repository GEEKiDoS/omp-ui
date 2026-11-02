#include "config.h"

#include "core/text_util.h"
#include "gfx/tween.h"
#include "text/spans.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <array>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace app {
namespace {

using json = nlohmann::json;

constexpr std::array<std::pair<std::string_view, Theme::Backdrop>, 3> backdrops{
    {{"none", Theme::Backdrop::None}, {"blur", Theme::Backdrop::Blur}, {"acrylic", Theme::Backdrop::Acrylic}}};
constexpr std::array<std::pair<std::string_view, Theme::Fit>, 4> fits{{{"cover", Theme::Fit::Cover},
                                                                       {"contain", Theme::Fit::Contain},
                                                                       {"tile", Theme::Fit::Tile},
                                                                       {"center", Theme::Fit::Center}}};

std::optional<SDL_Color> parseColor(std::string_view value) {
	if (!value.starts_with('#') || (value.size() != 7 && value.size() != 9))
		return std::nullopt;
	Uint32 bits = 0;
	for (const char ch : value.substr(1)) {
		int digit = -1;
		if (ch >= '0' && ch <= '9')
			digit = ch - '0';
		else if (ch >= 'a' && ch <= 'f')
			digit = ch - 'a' + 10;
		else if (ch >= 'A' && ch <= 'F')
			digit = ch - 'A' + 10;
		if (digit < 0)
			return std::nullopt;
		bits = bits << 4 | static_cast<Uint32>(digit);
	}
	if (value.size() == 7)
		return text::rgb(bits);
	return text::rgb(bits >> 8, static_cast<Uint8>(bits & 0xff));
}

std::string formatColor(SDL_Color color) {
	auto value = std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b);
	if (color.a != 255)
		value += std::format("{:02x}", color.a);
	return value;
}

json readJson(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	return input ? json::parse(input, nullptr, false) : json();
}

void writeJson(const std::filesystem::path& path, const json& value) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output << value.dump(2) << '\n';
}

// The built-in theme as a file, listing every setting.
json themeFile(bool dark) {
	text::usePalette(dark);
	json colors = json::object();
	for (const auto name : text::paletteNames)
		colors[std::string(name)] = formatColor(text::paletteColor(name)->get());
	return {
	    {"base", dark ? "dark" : "light"},
	    {"colors", std::move(colors)},
	    {"window", {{"opacity", 1.0}, {"backdrop", "none"}}},
	    {"background", {{"image", ""}, {"opacity", 0.15}, {"fit", "cover"}}},
	};
}

void writeDefaults(const std::filesystem::path& folder) {
	std::error_code error;
	if (std::filesystem::exists(folder / "config.json", error))
		return;
	std::filesystem::create_directories(folder / "themes", error);
	writeJson(folder / "config.json", {{"theme", "dark"}, {"animations", true}, {"notify", "notify"}});
	for (const bool dark : {true, false}) {
		const auto file = folder / "themes" / (dark ? "dark.json" : "light.json");
		if (!std::filesystem::exists(file, error))
			writeJson(file, themeFile(dark));
	}
}

template <typename Value, size_t Count>
Value named(const std::array<std::pair<std::string_view, Value>, Count>& names, const json& section, const char* key,
            Value fallback, const std::string& theme) {
	const auto value = section.value(key, std::string());
	if (value.empty())
		return fallback;
	for (const auto& [name, result] : names)
		if (name == value)
			return result;
	SDL_Log("Theme %s: unknown %s \"%s\"", theme.c_str(), key, value.c_str());
	return fallback;
}

void applyColors(const json& colors, const std::string& name) {
	if (!colors.is_object())
		return;
	for (const auto& [key, value] : colors.items()) {
		const auto target = text::paletteColor(key);
		const auto color = value.is_string() ? parseColor(value.get_ref<const std::string&>()) : std::nullopt;
		if (!target)
			SDL_Log("Theme %s: unknown colour \"%s\"", name.c_str(), key.c_str());
		else if (!color)
			SDL_Log("Theme %s: \"%s\" is not #rrggbb or #rrggbbaa", name.c_str(), key.c_str());
		else
			target->get() = *color;
	}
	// The canvas and sidebar follow an overridden bg/surface unless named themselves.
	if (!colors.contains("canvas"))
		text::palette::canvas = text::palette::bg;
	if (!colors.contains("sidebar"))
		text::palette::sidebar = text::palette::surface;
}

void applyWindow(const json& window, Theme& theme) {
	if (!window.is_object())
		return;
	theme.backdrop = named(backdrops, window, "backdrop", Theme::Backdrop::None, theme.name);
	// A backdrop only shows through a translucent window; default to a light veil.
	const float opacity =
	    std::clamp(window.value("opacity", theme.backdrop == Theme::Backdrop::None ? 1.0f : 0.75f), 0.0f, 1.0f);
	// Panels (cards, code, prompt, menus) stay more opaque than the canvas so text on them reads.
	const float panels = std::clamp(window.value("panelOpacity", (1.0f + opacity) * 0.5f), 0.0f, 1.0f);
	const auto scale = [](SDL_Color& color, float factor) {
		color.a = static_cast<Uint8>(std::lround(color.a * factor));
	};
	using namespace text::palette;
	for (SDL_Color& color : std::array{std::ref(canvas), std::ref(sidebar)})
		scale(color, opacity);
	for (SDL_Color& color : std::array{std::ref(surface), std::ref(surface2), std::ref(userCard), std::ref(hover),
	                                   std::ref(selected), std::ref(primary), std::ref(danger), std::ref(caution)})
		scale(color, panels);
}

void applyBackground(const json& background, const std::filesystem::path& folder, Theme& theme) {
	if (!background.is_object())
		return;
	if (const auto image = background.value("image", std::string()); !image.empty())
		theme.image = folder / text::utf8Path(image);
	theme.imageOpacity = std::clamp(background.value("opacity", theme.imageOpacity), 0.0f, 1.0f);
	theme.fit = named(fits, background, "fit", theme.fit, theme.name);
}

} // namespace

bool Theme::transparent() const {
	return text::palette::canvas.a < 255 || text::palette::sidebar.a < 255;
}

std::filesystem::path settingsFolder() {
	const std::unique_ptr<char, decltype(&SDL_free)> pref(SDL_GetPrefPath("omp", "omp-ui"), SDL_free);
	return pref ? text::utf8Path(pref.get()) : std::filesystem::path();
}

Theme applyTheme(bool userConfig, const std::filesystem::path& configFile) {
	const auto folder = settingsFolder();
	const bool found = !folder.empty();
	if (found && userConfig && configFile.empty())
		writeDefaults(folder);
	Theme theme;
	const auto config = !configFile.empty() ? readJson(configFile)
	                  : found && userConfig ? readJson(folder / "config.json")
	                                        : json();
	if (const char* forced = SDL_getenv("OMP_UI_THEME"); forced && *forced)
		theme.name = forced;
	else if (config.is_object() && config.contains("theme") && config["theme"].is_string())
		theme.name = config["theme"].get<std::string>();
	if (config.is_object() && config.contains("dimAfter")) {
		if (config["dimAfter"].is_number() && config["dimAfter"].get<double>() >= 0)
			theme.dimAfter = config["dimAfter"].get<double>();
		else
			SDL_Log("config.json: \"dimAfter\" is not a number of seconds >= 0");
	}
	if (config.is_object() && config.contains("animations")) {
		if (config["animations"].is_boolean())
			theme.animations = config["animations"].get<bool>();
		else
			SDL_Log("config.json: \"animations\" is not a boolean");
	}
	if (config.is_object() && config.contains("notify")) {
		const auto value = config["notify"].is_string() ? config["notify"].get<std::string>() : std::string();
		if (value == "silent")
			theme.notify = Theme::Notify::Silent;
		else if (value == "sound")
			theme.notify = Theme::Notify::Sound;
		else if (value == "notify")
			theme.notify = Theme::Notify::Notify;
		else
			SDL_Log("config.json: \"notify\" is not \"silent\", \"sound\" or \"notify\"");
	}
	gfx::animations = theme.animations;
	// Test runs keep the built-in palettes even when the user edited themes/dark.json.
	const bool builtin = theme.name == "dark" || theme.name == "light";
	json file;
	if (found && (userConfig || !builtin)) {
		const auto path = folder / "themes" / text::utf8Path(theme.name + ".json");
		std::error_code error;
		if (std::filesystem::exists(path, error)) {
			file = readJson(path);
			if (!file.is_object())
				SDL_Log("Theme %s: %s is not a JSON object", theme.name.c_str(), path.string().c_str());
		} else if (!builtin)
			SDL_Log("Theme %s: no %s", theme.name.c_str(), path.string().c_str());
	}
	if (!file.is_object()) {
		if (!builtin)
			theme.name = "dark";
		text::usePalette(theme.name == "dark");
		return theme;
	}
	const auto base = file.value("base", builtin ? theme.name : std::string("dark"));
	if (base != "dark" && base != "light")
		SDL_Log("Theme %s: unknown base \"%s\", using dark", theme.name.c_str(), base.c_str());
	text::usePalette(base != "light");
	applyColors(file.value("colors", json::object()), theme.name);
	applyWindow(file.value("window", json::object()), theme);
	applyBackground(file.value("background", json::object()), folder / "themes", theme);
	return theme;
}

} // namespace app
