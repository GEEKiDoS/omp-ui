#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstdlib>

#include "core/tsp.h"
#include "app/config.h"
#include "app/drive.h"
#include "app/input.h"
#include "view/view_tree.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>

namespace {

struct SdlLifetime {
	~SdlLifetime() { SDL_Quit(); }
};
struct TtfLifetime {
	~TtfLifetime() { TTF_Quit(); }
};
struct WindowDeleter {
	void operator()(SDL_Window* value) const { SDL_DestroyWindow(value); }
};
struct RendererDeleter {
	void operator()(SDL_Renderer* value) const { SDL_DestroyRenderer(value); }
};
struct Record {
	double at;
	std::string message;
};

bool load(const std::string& path, std::vector<Record>& records, std::string& error) {
	std::ifstream file(path);
	if (!file) {
		error = "Cannot open recording: " + path;
		return false;
	}
	std::string line;
	size_t lineNumber = 0;
	while (std::getline(file, line)) {
		++lineNumber;
		if (line.empty())
			continue;
		try {
			const auto record = json::parse(line);
			if (record.value("dir", "") != "out")
				continue;
			const auto verb = record.at("verb").get<std::string>();
			std::string message = "tsp;" + verb + ";";
			if (const auto params = record.find("params"); params != record.end())
				for (const auto& [key, value] : params->items())
					message += key + "=" + value.get<std::string>() + ";";
			const auto& body = record.at("body");
			message += verb == "b" ? body.get<std::string>() : body.dump();
			records.push_back({record.value("t", 0.0), std::move(message)});
		} catch (const json::exception& exception) {
			error = path + ":" + std::to_string(lineNumber) + ": " + exception.what();
			return false;
		}
	}
	if (records.empty()) {
		error = "Recording contains no dir:out messages";
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::cerr << "Usage: tsp-replay recording.jsonl [--drive script.txt] [--screenshot out.png] [--at frame] "
		             "[--speed factor] [--wait ms]\n";
		return 1;
	}
	std::string shot, drivePath;
	uint64_t target = std::numeric_limits<uint64_t>::max();
	double speed = 1;
	Uint32 waitMs = 0;
	for (int i = 2; i < argc; ++i) {
		const std::string_view option(argv[i]);
		if (i + 1 == argc) {
			std::cerr << "Missing value for " << option << '\n';
			return 1;
		}
		const std::string_view value(argv[++i]);
		if (value.empty()) {
			std::cerr << "Missing value for " << option << '\n';
			return 1;
		}
		if (option == "--screenshot")
			shot = value;
		else if (option == "--drive")
			drivePath = value;
		else if (option == "--at") {
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), target);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
				std::cerr << "Invalid frame\n";
				return 1;
			}
		} else if (option == "--wait") {
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), waitMs);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
				std::cerr << "Invalid wait duration\n";
				return 1;
			}
		} else if (option == "--speed") {
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), speed);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(speed)
			    || speed <= 0) {
				std::cerr << "Invalid speed\n";
				return 1;
			}
		} else {
			std::cerr << "Unknown option: " << option << '\n';
			return 1;
		}
	}
	std::vector<Record> records;
	std::string error;
	if (!load(argv[1], records, error)) {
		std::cerr << error << '\n';
		return 1;
	}
	SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
		std::cerr << SDL_GetError() << '\n';
		return 1;
	}
	app::applyTheme(false); // Replays use no user config or child environment overrides.
	SdlLifetime sdl;
	if (!TTF_Init()) {
		std::cerr << SDL_GetError() << '\n';
		return 1;
	}
	TtfLifetime ttf;
	// A realistic desktop size, fixed so screenshots are comparable between runs.
	std::unique_ptr<SDL_Window, WindowDeleter> window(SDL_CreateWindow(
	    "tsp-replay", 1600, 1000,
	    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
	        | ((!shot.empty() || !drivePath.empty()) ? SDL_WINDOW_HIDDEN | SDL_WINDOW_NOT_FOCUSABLE : 0)));
	if (!window) {
		std::cerr << SDL_GetError() << '\n';
		return 1;
	}
	std::unique_ptr<SDL_Renderer, RendererDeleter> renderer(SDL_CreateRenderer(window.get(), nullptr));
	if (!renderer) {
		std::cerr << SDL_GetError() << '\n';
		return 1;
	}
	SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_BLEND);
	text::Fonts fonts;
	float scale = SDL_GetWindowDisplayScale(window.get());
	if (const auto forced = SDL_getenv("OMP_UI_SCALE"))
		scale = std::max(0.5f, static_cast<float>(std::atof(forced)));
	if (!fonts.load(scale, error)) {
		std::cerr << error << '\n';
		return 1;
	}
	text::Engine engine(*renderer, fonts);
	if (!engine.valid()) {
		std::cerr << SDL_GetError() << '\n';
		return 1;
	}
	Pty pty;
	TerminalHost host([](const auto&) {});
	auto& session = host.session;
	app::Input input(*window, host, pty);
	view::Views views(session, *renderer, engine, fonts);
	size_t next = 0;
	const auto started = SDL_GetTicks();
	bool running = true, dirty = true, animating = false;
	input.searchFocused = [&] {
		return views.searchFocused();
	};
	input.claimSearchKey = [&](SDL_Keycode key, SDL_Keymod mod) {
		return views.claimSearchKey(key, mod);
	};
	input.searchKey = [&](SDL_Keycode key, SDL_Keymod mod, std::string_view typed) {
		const bool consumed = views.searchKey(key, mod, typed);
		dirty |= consumed;
		return consumed;
	};
	input.searchText = [&](std::string_view typed) {
		const bool consumed = views.searchText(typed);
		dirty |= consumed;
		return consumed;
	};
	auto resize = [&] {
		int w = 1, h = 1;
		SDL_GetRenderOutputSize(renderer.get(), &w, &h);
		session.resize(std::max(1, static_cast<int>(static_cast<float>(w) / fonts.monoAdvance())), fonts.monoAdvance(),
		               fonts.lineHeight());
	};
	resize();
	std::unique_ptr<app::Drive> drive;
	if (!drivePath.empty()) {
		drive = std::make_unique<app::Drive>(
		    drivePath, *window, *renderer, session, views,
		    [&](float value) {
			    scale = value;
			    engine.clear();
			    fonts.setScale(scale);
			    views.metricsChanged();
			    resize();
		    },
		    std::function<void()>{}, input);
		if (!drive->error().empty()) {
			std::cerr << drive->error() << '\n';
			return 1;
		}
	}
	std::optional<Uint64> captureAt;
	while (running) {
		const double elapsed = static_cast<double>(SDL_GetTicks() - started) * speed;
		while (next < records.size() && session.framesApplied < target
		       && (!shot.empty() || drive || target != std::numeric_limits<uint64_t>::max()
		           || records[next].at - records.front().at <= elapsed)) {
			if (!session.onApc(records[next].message)) {
				std::cerr << "Unrecognized record " << next + 1 << '\n';
				return 1;
			}
			++next;
			dirty = true;
		}
		if (!shot.empty() && !captureAt)
			captureAt = SDL_GetTicks() + waitMs;
		if (captureAt && SDL_GetTicks() >= *captureAt)
			dirty = true;
		if (dirty || animating) {
			int w = 1, h = 1;
			SDL_GetRenderOutputSize(renderer.get(), &w, &h);
			SDL_SetRenderDrawColor(renderer.get(), text::palette::bg.r, text::palette::bg.g, text::palette::bg.b, 255);
			SDL_RenderClear(renderer.get());
			animating = views.paint({0, 0, static_cast<float>(w), static_cast<float>(h)});
			animating |= views.paintImageViewer();
			int ww = 1, wh = 1;
			SDL_GetWindowSize(window.get(), &ww, &wh);
			input.setCaret(views.focusedCaret(), static_cast<float>(w) / static_cast<float>(ww),
			               static_cast<float>(h) / static_cast<float>(wh));
			if (captureAt && SDL_GetTicks() >= *captureAt) {
				if (!app::saveScreenshot(*renderer, shot)) {
					std::cerr << "Screenshot failed: " << SDL_GetError() << '\n';
					return 1;
				}
				running = false;
			}
			if (drive)
				drive->beforePresent();
			SDL_RenderPresent(renderer.get());
			session.flushAcks();
			dirty = false;
		}
		if (drive)
			dirty |= drive->tick(running);
		if (!running)
			break;
		int timeout = animating ? (views.transitioning() ? 16 : 33) : -1;
		const int hoverDelay = views.wakeDelay();
		if (hoverDelay >= 0)
			timeout = timeout < 0 ? hoverDelay : std::min(timeout, hoverDelay);
		if (drive)
			timeout = timeout < 0 ? drive->wakeDelay() : std::min(timeout, drive->wakeDelay());
		if (captureAt) {
			const auto now = SDL_GetTicks();
			const int until = static_cast<int>(
			    std::min<Uint64>(*captureAt > now ? *captureAt - now : 0, std::numeric_limits<int>::max()));
			timeout = timeout < 0 ? until : std::min(timeout, until);
		}
		if (next < records.size() && session.framesApplied < target) {
			const int until =
			    std::max(0, static_cast<int>(std::ceil((records[next].at - records.front().at - elapsed) / speed)));
			timeout = timeout < 0 ? until : std::min(timeout, until);
		}
		SDL_Event event{};
		if (SDL_WaitEventTimeout(&event, timeout))
			do {
				if (event.type == SDL_EVENT_QUIT
				    || (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE
				        && !views.claimSearchKey(event.key.key, event.key.mod))) {
					running = false;
					break;
				}
				if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
					resize();
					dirty = true;
				}
				if (event.type == SDL_EVENT_WINDOW_EXPOSED)
					dirty = true;
				int ww = 1, wh = 1, ow = 1, oh = 1;
				SDL_GetWindowSize(window.get(), &ww, &wh);
				SDL_GetRenderOutputSize(renderer.get(), &ow, &oh);
				dirty |= views.handle(event, static_cast<float>(ow) / static_cast<float>(ww),
				                      static_cast<float>(oh) / static_cast<float>(wh));
				input.handle(event);
			} while (SDL_PollEvent(&event));
		dirty |= views.tick() != view::Views::Wake::None;
	}
	std::cout << "frames " << session.framesApplied << " ops " << session.opsApplied << " rejected " << session.opErrors
	          << '\n';
	if (target != std::numeric_limits<uint64_t>::max() && session.framesApplied < target) {
		std::cerr << "Recording ended before frame " << target << '\n';
		return 1;
	}
	return session.opErrors == 0 && (!drive || drive->error().empty()) ? 0 : 1;
}
