#include "drive.h"
#include "input.h"
#include "view/view_tree.h"
#include "core/text_util.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <span>

namespace app {
namespace {
struct SurfaceDeleter {
	void operator()(SDL_Surface* value) const { SDL_DestroySurface(value); }
};
void bigEndian(std::vector<Uint8>& bytes, Uint32 value) {
	for (int shift = 24; shift >= 0; shift -= 8)
		bytes.push_back(static_cast<Uint8>(value >> shift));
}
void chunk(std::vector<Uint8>& png, std::string_view name, std::span<const Uint8> data) {
	bigEndian(png, static_cast<Uint32>(data.size()));
	const size_t begin = png.size();
	png.insert(png.end(), name.begin(), name.end());
	png.insert(png.end(), data.begin(), data.end());
	Uint32 crc = 0xffffffff;
	for (size_t i = begin; i < png.size(); ++i) {
		crc ^= png[i];
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	bigEndian(png, crc ^ 0xffffffff);
}
std::string unescape(std::string_view source) {
	std::string result;
	result.reserve(source.size());
	for (size_t i = 0; i < source.size(); ++i) {
		if (source[i] == '\\' && i + 1 < source.size()) {
			const char next = source[i + 1];
			if (next == 'n' || next == 'r' || next == 't' || next == '\\') {
				result += next == 'n' ? '\n' : next == 'r' ? '\r' : next == 't' ? '\t' : '\\';
				++i;
				continue;
			}
		}
		result += source[i];
	}
	return result;
}
void strings(const json& value, std::string& result) {
	if (value.is_string()) {
		result += value.get_ref<const std::string&>();
		result += '\n';
	} else if (value.is_structured())
		for (const auto& child : value)
			strings(child, result);
}
} // namespace

// Stored DEFLATE blocks keep PNG output dependency-free (SDL3 has no PNG writer).
bool saveScreenshot(SDL_Renderer& renderer, const std::string& path) {
	std::unique_ptr<SDL_Surface, SurfaceDeleter> pixels(SDL_RenderReadPixels(&renderer, nullptr));
	if (!pixels)
		return false;
	std::unique_ptr<SDL_Surface, SurfaceDeleter> rgba(SDL_ConvertSurface(pixels.get(), SDL_PIXELFORMAT_RGBA32));
	if (!rgba)
		return false;
	const auto source = static_cast<const Uint8*>(rgba->pixels);
	std::vector<Uint8> scanlines;
	scanlines.reserve((static_cast<size_t>(rgba->w) * 4 + 1) * static_cast<size_t>(rgba->h));
	for (int y = 0; y < rgba->h; ++y) {
		scanlines.push_back(0);
		const auto row = std::span(source + static_cast<size_t>(y) * static_cast<size_t>(rgba->pitch),
		                           static_cast<size_t>(rgba->w) * 4);
		scanlines.insert(scanlines.end(), row.begin(), row.end());
	}
	std::vector<Uint8> deflate{0x78, 0x01};
	deflate.reserve(scanlines.size() + scanlines.size() / 65535 * 5 + 16);
	Uint32 a = 1, b = 0;
	for (size_t at = 0; at < scanlines.size();) {
		const auto count = static_cast<Uint16>(std::min<size_t>(65535, scanlines.size() - at));
		deflate.insert(deflate.end(),
		               {static_cast<Uint8>(at + count == scanlines.size() ? 1 : 0), static_cast<Uint8>(count),
		                static_cast<Uint8>(count >> 8), static_cast<Uint8>(~count), static_cast<Uint8>(~count >> 8)});
		for (const auto value : std::span(scanlines).subspan(at, count)) {
			deflate.push_back(value);
			a = (a + value) % 65521;
			b = (b + a) % 65521;
		}
		at += count;
	}
	bigEndian(deflate, (b << 16) | a);
	std::vector<Uint8> png{137, 80, 78, 71, 13, 10, 26, 10};
	std::vector<Uint8> header;
	bigEndian(header, static_cast<Uint32>(rgba->w));
	bigEndian(header, static_cast<Uint32>(rgba->h));
	header.insert(header.end(), {8, 6, 0, 0, 0});
	chunk(png, "IHDR", header);
	chunk(png, "IDAT", deflate);
	chunk(png, "IEND", {});
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
	return static_cast<bool>(file);
}

Drive::Drive(const std::string& path, SDL_Window& window, SDL_Renderer& renderer, tsp::Session& session,
             view::Views& views, std::function<void(float)> scale, std::function<void()> inspector, Input& input)
    : window_(window),
      renderer_(renderer),
      session_(session),
      views_(views),
      scale_(std::move(scale)),
      inspector_(std::move(inspector)),
      input_(input),
      path_(path) {
	views_.get().setSystemPointer(false);
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		fail("Cannot open script");
		return;
	}
	std::string line;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (lines_.empty() && line.starts_with("\xef\xbb\xbf"))
			line.erase(0, 3);
		lines_.push_back(std::move(line));
	}
	if (file.bad())
		fail("Cannot read script");
}
void Drive::setTarget(tsp::Session& session, view::Views& views) {
	session_ = session;
	views_ = views;
	views_.get().setSystemPointer(false);
}
void Drive::setWindow(SDL_Window& window, SDL_Renderer& renderer, Input& input) {
	window_ = window;
	renderer_ = renderer;
	input_ = input;
}
void Drive::fail(std::string message) {
	error_ = path_ + ":" + std::to_string(next_) + ": " + message;
	SDL_Log("%s", error_.c_str());
}
int Drive::wakeDelay() const {
	if (!deadline_)
		return 0;
	const auto now = SDL_GetTicks();
	const auto delay = std::min<Uint64>(deadline_ > now ? deadline_ - now : 0, std::numeric_limits<int>::max());
	return static_cast<int>(pattern_ ? std::min<Uint64>(delay, 25) : delay);
}
bool Drive::matches() const {
	std::string text;
	for (const auto& entry : session_.get().log) {
		text += entry.verb;
		text += ' ';
		text += entry.text;
		text += '\n';
	}
	for (const auto& surface : session_.get().surfaces()) {
		const auto snapshot = surface->doc->snapshot();
		text += snapshot.dump();
		text += '\n';
		strings(snapshot, text);
	}
	return std::regex_search(text, *pattern_);
}
bool Drive::dump(const std::string& path, bool nodes) {
	std::ofstream file(path, std::ios::binary);
	if (nodes) {
		json output = json::array();
		for (const auto& surface : session_.get().surfaces()) {
			json entries = json::array();
			auto visit = [&](auto&& self, tsp::NodeId id) -> void {
				const auto& node = surface->doc->at(id);
				json value{{"id", node.id}, {"k", tsp::kindName(node.kind)}, {"p", node.p}};
				if (const auto rect = views_.get().nodeRect(surface->id, node.id))
					value["rect"] = {rect->x, rect->y, rect->w, rect->h};
				if (const auto caret = views_.get().caretRect(surface->id, node.id))
					value["caret"] = {caret->x, caret->y, caret->w, caret->h};
				entries.push_back(std::move(value));
				for (const auto child : node.children)
					self(self, child);
			};
			visit(visit, surface->doc->root());
			output.push_back({{"id", surface->id}, {"open", surface->open}, {"nodes", std::move(entries)}});
		}
		file << output.dump(2) << '\n';
	} else
		for (const auto& entry : session_.get().log)
			file << (entry.out ? "-> " : "<- ") << entry.verb << ' ' << entry.text << '\n';
	if (!file) {
		fail("Cannot write " + path);
		return false;
	}
	return true;
}
void Drive::pointer(Uint32 type, float x, float y, int button, int clicks, float lines) {
	SDL_Event event{};
	event.type = type;
	if (type == SDL_EVENT_MOUSE_MOTION) {
		event.motion.windowID = SDL_GetWindowID(&window_.get());
		event.motion.x = x;
		event.motion.y = y;
	} else if (type == SDL_EVENT_MOUSE_WHEEL) {
		event.wheel.windowID = SDL_GetWindowID(&window_.get());
		event.wheel.mouse_x = x;
		event.wheel.mouse_y = y;
		event.wheel.y = lines;
	} else {
		event.button.windowID = SDL_GetWindowID(&window_.get());
		event.button.x = x;
		event.button.y = y;
		event.button.button = static_cast<Uint8>(button);
		event.button.clicks = static_cast<Uint8>(clicks);
	}
	if (!eventHandler_ || !eventHandler_(event))
		views_.get().handle(event, 1, 1);
}
void Drive::beforePresent() {
	if (pendingShot_.empty())
		return;
	if (!saveScreenshot(renderer_.get(), pendingShot_))
		fail("Screenshot failed: " + pendingShot_ + ": " + SDL_GetError());
	pendingShot_.clear();
}
bool Drive::tick(bool& running) {
	if (!error_.empty()) {
		running = false;
		return false;
	}
	if (!pendingShot_.empty())
		return true;
	if (deadline_) {
		if (pattern_ && matches()) {
			pattern_.reset();
			deadline_ = 0;
		} else if (SDL_GetTicks() < deadline_)
			return false;
		else if (pattern_) {
			fail("waitfor timed out");
			running = false;
			return false;
		} else
			deadline_ = 0;
	}
	while (next_ < lines_.size()) {
		const auto& line = lines_[next_++];
		const auto first = line.find_first_not_of(" \t");
		if (first == std::string::npos || line[first] == '#')
			continue;
		const auto split = line.find_first_of(" \t", first);
		const auto command = line.substr(first, split == std::string::npos ? split : split - first);
		const auto start = split == std::string::npos ? split : split + 1;
		const std::string arg = start == std::string::npos ? "" : line.substr(start);
		std::istringstream values(arg);
		float x = 0, y = 0, z = 0;
		Uint64 ms = 0;
		bool valid = true;
		if (command == "wait" || command == "waitfor") {
			valid = static_cast<bool>(values >> ms) && ms <= std::numeric_limits<int>::max();
			if (valid) {
				deadline_ = SDL_GetTicks() + ms;
				if (command == "waitfor") {
					std::string expression;
					std::getline(values >> std::ws, expression);
					try {
						pattern_.emplace(expression, std::regex::ECMAScript);
					} catch (const std::regex_error& exception) {
						fail(exception.what());
					}
					valid = !expression.empty();
				}
			}
		} else if (command == "text" || command == "paste") {
			SDL_Event event{};
			event.type = SDL_EVENT_TEXT_INPUT;
			event.text.windowID = SDL_GetWindowID(&window_.get());
			views_.get().handle(event, 1, 1);
			if (command == "text")
				input_.get().driveText(unescape(arg));
			else
				input_.get().paste(unescape(arg));
		} else if (command == "key") {
			SDL_Event event{};
			event.type = SDL_EVENT_KEY_DOWN;
			event.key.windowID = SDL_GetWindowID(&window_.get());
			views_.get().handle(event, 1, 1);
			if ((arg == "f12" || arg == "F12") && inspector_ && !input_.get().onShortcut)
				inspector_();
			else {
				std::string error;
				if (!input_.get().driveKey(arg, error))
					fail(error);
			}
		} else if (command == "drop") {
			valid = !arg.empty();
			if (valid && !eventHandler_)
				fail("drop needs omp-ui");
			else if (valid) {
				SDL_Event event{};
				event.type = SDL_EVENT_DROP_FILE;
				event.drop.windowID = SDL_GetWindowID(&window_.get());
				event.drop.data = arg.c_str();
				eventHandler_(event);
			}
		} else if (command == "tab" || command == "tabbg" || command == "tabcmd" || command == "tabs"
		           || command == "window" || command == "newwindow" || command == "merge" || command == "tabdrag"
		           || command == "idle") {
			valid = command == "newwindow" || command == "merge" || command == "idle" || !arg.empty();
			if (valid) {
				if (!tabAction_)
					fail("Tab actions are unavailable");
				else if (auto error = tabAction_(command, arg); !error.empty())
					fail(std::move(error));
			}
		} else if (command == "move" || command == "click" || command == "dblclick" || command == "rclick"
		           || command == "mclick" || command == "wheel") {
			valid = static_cast<bool>(values >> x >> y) && std::isfinite(x) && std::isfinite(y);
			if (command == "wheel")
				valid = valid && static_cast<bool>(values >> z) && std::isfinite(z);
			if (valid) {
				pointer(SDL_EVENT_MOUSE_MOTION, x, y);
				if (command == "wheel")
					pointer(SDL_EVENT_MOUSE_WHEEL, x, y, 0, 0, z);
				else if (command != "move") {
					const int button = command == "rclick" ? SDL_BUTTON_RIGHT
					                 : command == "mclick" ? SDL_BUTTON_MIDDLE
					                                       : SDL_BUTTON_LEFT;
					pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, button, 1);
					pointer(SDL_EVENT_MOUSE_BUTTON_UP, x, y, button, 1);
					if (command == "dblclick") {
						pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, button, 2);
						pointer(SDL_EVENT_MOUSE_BUTTON_UP, x, y, button, 2);
					}
				}
			}
		} else if (command == "drag") {
			float x2 = 0, y2 = 0;
			valid = static_cast<bool>(values >> x >> y >> x2 >> y2) && std::isfinite(x) && std::isfinite(y)
			     && std::isfinite(x2) && std::isfinite(y2);
			if (valid) {
				pointer(SDL_EVENT_MOUSE_MOTION, x, y);
				pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT, 1);
				pointer(SDL_EVENT_MOUSE_MOTION, (x + x2) * 0.5f, (y + y2) * 0.5f);
				pointer(SDL_EVENT_MOUSE_MOTION, x2, y2);
				pointer(SDL_EVENT_MOUSE_BUTTON_UP, x2, y2, SDL_BUTTON_LEFT, 1);
			}
		} else if (command == "resize") {
			valid = static_cast<bool>(values >> x >> y) && std::isfinite(x) && std::isfinite(y) && x >= 1 && y >= 1
			     && x <= 32768 && y <= 32768;
			if (valid) {
				int ww = 1, wh = 1, ow = 1, oh = 1;
				SDL_GetWindowSize(&window_.get(), &ww, &wh);
				SDL_GetRenderOutputSize(&renderer_.get(), &ow, &oh);
				if (!SDL_SetWindowSize(&window_.get(), static_cast<int>(std::lround(x * ww / ow)),
				                       static_cast<int>(std::lround(y * wh / oh))))
					fail(SDL_GetError());
			}
		} else if (command == "scale") {
			valid = static_cast<bool>(values >> x) && std::isfinite(x) && x >= 0.5f && x <= 8;
			if (valid)
				scale_(x);
		} else if (command == "shot") {
			valid = !arg.empty();
			if (valid)
				pendingShot_ = arg;
		} else if (command == "log" || command == "nodes") {
			valid = !arg.empty();
			if (valid)
				dump(arg, command == "nodes");
		} else if (command == "reveal") {
			// Scrolls the transcript so a node (by id, in any surface holding it) starts at the top.
			valid = !arg.empty();
			if (valid) {
				bool found = false;
				for (const auto& surface : session_.get().surfaces())
					if (surface->doc->find(arg)) {
						surface->doc->reveals.emplace_back(arg, "start");
						found = true;
					}
				if (!found)
					fail("No node " + arg);
			}
		} else if (command == "selection") {
			// The text Ctrl/Cmd+C would copy, without touching the user's clipboard.
			valid = !arg.empty();
			if (valid) {
				std::ofstream file(text::utf8Path(arg), std::ios::binary);
				file << views_.get().selectionText();
				if (!file)
					fail("Cannot write " + arg);
			}
		} else if (command == "quit")
			running = false;
		else {
			fail("Unknown step: " + command);
			valid = true;
		}
		if (!valid)
			fail("Invalid arguments for " + command);
		if (!error_.empty())
			running = false;
		return true;
	}
	running = false;
	return false;
}
} // namespace app
