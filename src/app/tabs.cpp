#include "tabs.h"
#include "config.h"
#include "core/text_util.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <utility>

namespace app {
namespace {
void label(text::Engine& engine, std::string value, float x, float y, float width, text::RunStyle style = {}) {
	text::Runs runs{{std::move(value), std::move(style)}};
	const auto layout = engine.layout(runs, {.width = std::max(1.0f, width), .wrap = text::Wrap::None, .maxLines = 1});
	engine.draw(runs, layout, x, y);
}
bool inside(float x, float y, const SDL_FRect& rect) {
	return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}
} // namespace

void Inbox::wake() const {
	SDL_Event value{};
	value.type = event;
	SDL_PushEvent(&value);
}
void Inbox::append(std::string_view value) {
	bool notify;
	{
		std::scoped_lock lock(mutex);
		bytes.emplace_back(value);
		notify = !pending;
		pending = true;
	}
	if (notify)
		wake();
}
void Inbox::exit(int code) {
	{
		std::scoped_lock lock(mutex);
		exited = code;
	}
	wake();
}
bool Inbox::drain(TerminalHost& host, std::optional<int>& exitCode) {
	std::vector<std::string> batch;
	{
		std::scoped_lock lock(mutex);
		batch.swap(bytes);
		exitCode = std::exchange(exited, {});
		pending = false;
	}
	for (const auto& value : batch)
		host.feed(value);
	return !batch.empty();
}

Tab::Tab(std::string cwdValue, std::string titleValue, std::vector<std::string> commandValue, Uint32 wakeEvent,
         SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts)
    : host([this](const auto& bytes) { pty.write(bytes); }),
      views(host.session, renderer, engine, fonts),
      inbox{.event = wakeEvent},
      cwd(std::move(cwdValue)),
      title(std::move(titleValue)),
      command(std::move(commandValue)) {
	if (title.empty())
		title = folderName(cwd);
	applyTheme();
	host.session.onLog = [this](const tsp::LogEntry&) {
		++logGeneration;
	};
	host.onNotification = [this](std::string_view message) {
		if (!visible)
			attention = true;
		notification = std::string(message);
	};
	views.onRunningChanged = [this](int count) {
		if (!visible && running > 0 && count == 0)
			attention = true;
		running = count;
	};
}
void Tab::applyTheme() {
	// omp learns the theme from `hello` and from OSC 10/11 colour queries.
	const auto osc = [](SDL_Color color) {
		return std::format("rgb:{0:02x}{0:02x}/{1:02x}{1:02x}/{2:02x}{2:02x}", color.r, color.g, color.b);
	};
	host.session.dark = text::darkPalette;
	host.background = osc(text::palette::bg);
	host.foreground = osc(text::palette::fg);
}
Tab::~Tab() {
	pty.close(); // Reader callbacks must finish before the inbox and host disappear.
	views.onRunningChanged = {};
}
bool Tab::start(int cols, int rows, std::string& error) {
	host.rows = rows;
	host.session.resize(cols, host.session.cellW, host.session.cellH);
	if (!pty.start(
	        command, cwd, cols, rows, [this](auto value) { inbox.append(value); },
	        [this](int code) { inbox.exit(code); }, error)) {
		exited = true;
		exitCode = -1;
		host.rowsText = error;
		return false;
	}
	return true;
}
bool Tab::drain() {
	const bool helloAnswered = host.session.helloAnswered();
	std::optional<int> code;
	bool changed = inbox.drain(host, code);
	if (!helloAnswered && host.session.helloAnswered())
		host.session.sendEvent({{"ev", "visible"}, {"visible", visible}});
	if (code) {
		exited = true;
		exitCode = *code;
		if (!visible)
			attention = true;
		changed = true;
	}
	if (!host.title.empty() && title != host.title) {
		title = host.title;
		++titleGeneration;
		changed = true;
	}
	if (!visible)
		host.session.flushAcks();
	return changed;
}
void Tab::setVisible(bool value) {
	visible = value;
	if (visible)
		attention = false;
	if (host.session.helloAnswered())
		host.session.sendEvent({{"ev", "visible"}, {"visible", value}});
	if (!visible)
		host.session.flushAcks();
}
std::string folderName(std::string_view cwd) {
	const auto path = text::utf8Path(cwd);
	const auto name = text::pathText(path.filename());
	return name.empty() ? std::string(cwd) : name;
}
std::string currentFolder() {
	std::error_code error;
	return text::pathText(std::filesystem::current_path(error));
}

namespace {
constexpr size_t recentLimit = 8;
std::string normalFolder(const std::string& folder) {
	std::error_code error;
	const auto path = std::filesystem::weakly_canonical(text::utf8Path(folder), error);
	return error ? folder : text::pathText(path);
}
} // namespace

RecentFolders::RecentFolders(bool persist) {
	if (!persist)
		return;
	const auto folder = settingsFolder();
	if (folder.empty())
		return;
	file_ = text::pathText(folder / "recent.json");
	std::ifstream input(text::utf8Path(file_), std::ios::binary);
	const auto stored = json::parse(input, nullptr, false);
	if (!stored.is_array())
		return;
	std::error_code error;
	for (const auto& item : stored)
		if (item.is_string() && std::filesystem::is_directory(text::utf8Path(item.get<std::string>()), error))
			folders_.push_back(item.get<std::string>());
}

void RecentFolders::add(const std::string& folder) {
	const auto normal = normalFolder(folder);
	std::erase(folders_, normal);
	folders_.insert(folders_.begin(), normal);
	if (folders_.size() > recentLimit)
		folders_.resize(recentLimit);
	if (file_.empty())
		return;
	std::ofstream output(text::utf8Path(file_), std::ios::binary | std::ios::trunc);
	output << json(folders_).dump(1, '\t') << '\n';
}

void Sidebar::toggle() {
	collapsed = !collapsed;
	expansion.to(collapsed ? 0.0f : 1.0f, SDL_GetTicks(), 180);
	hovered.reset();
}
float Sidebar::width(const text::Fonts& fonts, Uint64 now) const {
	return fonts.size() * (3 + 13 * expansion.value(now));
}
float Sidebar::targetWidth(const text::Fonts& fonts) const {
	return fonts.size() * (collapsed ? 3 : 16);
}
float Sidebar::rowHeight(const text::Fonts& fonts) const {
	return fonts.size() * (collapsed ? 3 : 4.25f);
}
float Sidebar::headerHeight(const text::Fonts& fonts) const {
	return fonts.size() * 3;
}
float Sidebar::plusY(const text::Fonts& fonts, float height, size_t count) const {
	return std::min(height - fonts.size() * 3, headerHeight(fonts) + static_cast<float>(count) * rowHeight(fonts));
}
SidebarHit Sidebar::hit(float x, float y, int button, const text::Fonts& fonts, float height, size_t count,
                        float visibleWidth, bool moving) const {
	const float em = fonts.size(), header = headerHeight(fonts), plus = plusY(fonts, height, count);
	if (x < 0 || x >= visibleWidth || y < 0 || y >= height)
		return {};
	// Input uses the last presented width and transition state, not a newer tween sample.
	// Only the fixed-height toggle stays interactive until the final geometry is painted.
	if (moving)
		return {button == SDL_BUTTON_LEFT && y < header ? SidebarAction::Toggle : SidebarAction::None};
	const auto onButton = [button](SidebarAction left, SidebarAction right) {
		return button == SDL_BUTTON_LEFT ? left : button == SDL_BUTTON_RIGHT ? right : SidebarAction::None;
	};
	if (y < header)
		return {onButton(SidebarAction::Toggle, SidebarAction::HeaderMenu)};
	if (y >= plus)
		return {onButton(SidebarAction::New, SidebarAction::NewMenu)};
	const auto index = static_cast<size_t>((y - header + scroll) / rowHeight(fonts));
	if (index >= count)
		return {};
	if (button == SDL_BUTTON_MIDDLE || (button == SDL_BUTTON_LEFT && !collapsed && x >= visibleWidth - 2 * em))
		return {SidebarAction::Close, index};
	return {onButton(SidebarAction::Select, SidebarAction::TabMenu), index};
}
bool Sidebar::motion(float x, float y, const text::Fonts& fonts, float height, size_t count, float visibleWidth,
                     bool moving) {
	const auto target = hit(x, y, SDL_BUTTON_LEFT, fonts, height, count, visibleWidth, moving);
	const auto row = target.action == SidebarAction::Select || target.action == SidebarAction::Close
	                   ? std::optional<size_t>(target.index)
	                   : std::nullopt;
	const bool changed = hovered != row;
	if (changed || std::abs(x - pointerX) + std::abs(y - pointerY) > fonts.size() * 0.2f)
		hoverSince = SDL_GetTicks();
	hovered = row;
	pointerX = x;
	pointerY = y;
	return changed;
}
bool Sidebar::wheel(float delta, const text::Fonts& fonts, float height, size_t count, bool moving) {
	if (moving)
		return false;
	const float max = std::max(0.0f, static_cast<float>(count) * rowHeight(fonts)
	                                     - (plusY(fonts, height, count) - headerHeight(fonts)));
	const float next = std::clamp(scroll - delta * rowHeight(fonts), 0.0f, max);
	const bool changed = next != scroll;
	scroll = next;
	hovered.reset();
	return changed;
}
void Sidebar::reveal(size_t index, const text::Fonts& fonts, float height, size_t count) {
	const float row = rowHeight(fonts), available = plusY(fonts, height, count) - headerHeight(fonts);
	const float top = static_cast<float>(index) * row;
	if (top < scroll)
		scroll = top;
	else if (top + row > scroll + available)
		scroll = std::max(0.0f, top + row - available);
}
bool Sidebar::paint(SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts,
                    std::span<const std::unique_ptr<Tab>> tabs, size_t active, float height, float visibleWidth,
                    bool translucent) const {
	const float em = fonts.size(), w = targetWidth(fonts), header = headerHeight(fonts), row = rowHeight(fonts),
	            plus = plusY(fonts, height, tabs.size()), offset = visibleWidth - w;
	gfx::ScopedClip sidebarClip(renderer, {0, 0, visibleWidth, height});
	// Replace rather than blend, so a translucent sidebar keeps its own alpha over the canvas.
	const auto fill =
	    text::premultiplied(translucent ? text::palette::sidebar : text::withAlpha(text::palette::sidebar, 255));
	const SDL_FRect area{0, 0, visibleWidth, height};
	SDL_SetRenderDrawBlendMode(&renderer, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(&renderer, fill.r, fill.g, fill.b, fill.a);
	SDL_RenderFillRect(&renderer, &area);
	SDL_SetRenderDrawBlendMode(&renderer, SDL_BLENDMODE_BLEND);
	gfx::line(renderer, {visibleWidth - 1, 0}, {visibleWidth - 1, height}, text::palette::border);
	// Labels keep the destination width; only their origin and the enclosing clip move.
	label(engine, collapsed ? "☰" : "omp", offset + em, (header - fonts.lineHeight()) * 0.5f, w - 2 * em,
	      {.font = text::FontId::Bold, .color = text::palette::fg});
	if (!collapsed)
		label(engine, "‹", offset + w - 2 * em, (header - fonts.lineHeight()) * 0.5f, em,
		      {.color = text::palette::muted});
	gfx::ClipStack clip(renderer);
	clip.push({offset, header, w, std::max(0.0f, plus - header)});
	bool animating = false;
	for (size_t i = 0; i < tabs.size(); ++i) {
		const float y = header + static_cast<float>(i) * row - scroll;
		if (y + row <= header || y >= plus)
			continue;
		const auto& tab = *tabs[i];
		if (i == active || hovered == i)
			gfx::rounded(renderer, {offset + em * 0.4f, y + em * 0.2f, w - em * 0.8f, row - em * 0.4f}, em * 0.45f,
			             i == active ? text::palette::surface2 : text::palette::hover);
		if (i == active)
			gfx::rounded(renderer, {offset, y + em * 0.75f, em * 0.18f, row - em * 1.5f}, em * 0.09f,
			             text::palette::accent);
		// Expanded rows start their text at the header's inset; the state (running, attention,
		// exited) sits at the right end, where hovering shows the close button instead.
		const bool closeShown = !collapsed && hovered == i;
		const SDL_FPoint center{offset + (collapsed ? em * 1.5f : w - em * 1.25f), y + row * 0.5f};
		if (closeShown)
			label(engine, "×", center.x - em * 0.5f, y + (row - fonts.lineHeight()) * 0.5f, em,
			      {.color = text::palette::muted});
		else if (tab.running > 0 && !tab.exited) {
			const float phase = static_cast<float>(SDL_GetTicks() % 1000) * 0.0062831853f;
			gfx::arc(renderer, center, em * 0.38f, phase, phase + 4.3f, text::palette::accent, em * 0.12f);
			animating = true;
		} else if (tab.attention)
			gfx::rounded(renderer, {center.x - em * 0.22f, center.y - em * 0.22f, em * 0.44f, em * 0.44f}, em * 0.22f,
			             text::palette::accent);
		else if (tab.exited)
			label(engine, "×", center.x - em * 0.5f, y + (row - fonts.lineHeight()) * 0.5f, em,
			      {.color = text::palette::error});
		else if (collapsed)
			label(engine, std::to_string(i + 1), offset + em, y + (row - fonts.lineHeight()) * 0.5f, em,
			      {.color = i == active ? text::palette::fg : text::palette::dim});
		if (!collapsed) {
			const float x = offset + em, textWidth = w - em * 3.5f;
			label(engine, tab.title, x, y + em * 0.55f, textWidth,
			      {.font = i == active ? text::FontId::Bold : text::FontId::Ui});
			label(engine, folderName(tab.cwd), x, y + em * 0.65f + fonts.lineHeight(), textWidth,
			      {.color = text::palette::muted});
		}
	}
	clip.pop();
	gfx::line(renderer, {offset + em * 0.75f, plus}, {offset + w - em * 0.75f, plus}, text::palette::border);
	label(engine, collapsed ? "+" : "+  New tab", offset + em, plus + (em * 3 - fonts.lineHeight()) * 0.5f, w - em * 2,
	      {.color = text::palette::muted});
	return animating;
}
void Sidebar::paintTooltip(SDL_Renderer& renderer, text::Engine& engine, const text::Fonts& fonts,
                           std::span<const std::unique_ptr<Tab>> tabs, float outputWidth, float outputHeight,
                           bool moving) const {
	if (moving || !hovered || *hovered >= tabs.size() || SDL_GetTicks() < hoverSince + 650)
		return;
	const auto& path = tabs[*hovered]->cwd;
	const float em = fonts.size();
	text::Runs runs{{path, {.color = text::palette::fg}}};
	const float w = std::min(outputWidth - em * 2, fonts.width(text::FontId::Ui, path) + em * 1.5f);
	const auto layout = engine.layout(runs, {.width = std::max(1.0f, w - em * 1.5f)});
	const float h = layout.height + em,
	            x = std::clamp(targetWidth(fonts) + em * 0.5f, em, std::max(em, outputWidth - w - em));
	const float y = std::clamp(pointerY + em, em, std::max(em, outputHeight - h - em));
	gfx::rounded(renderer, {x, y, w, h}, em * 0.4f, text::palette::surface2);
	gfx::roundedBorder(renderer, {x, y, w, h}, em * 0.4f, text::palette::border);
	engine.draw(runs, layout, x + em * 0.75f, y + em * 0.5f);
}
int Sidebar::wakeDelay() const {
	if (!hovered)
		return -1;
	const auto now = SDL_GetTicks();
	return now < hoverSince + 650 ? static_cast<int>(hoverSince + 650 - now) : -1;
}

} // namespace app
