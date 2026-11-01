#include "code.h"

#include "gfx/draw.h"
#include "view/hit_targets.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace kinds {
namespace {

constexpr float naturalWidth = 1000000;
using LineKind = CodeCache::LineKind;

text::RunStyle mono(SDL_Color color = text::palette::fg) {
	text::RunStyle result;
	result.font = text::FontId::Mono;
	result.color = color;
	return result;
}

SDL_Color translucent(SDL_Color color, Uint8 alpha) {
	color.a = alpha;
	return color;
}

void add(text::Runs& runs, std::string_view value, const text::RunStyle& style) {
	if (!value.empty())
		runs.push_back({std::string(value), style});
}

void replace(text::Runs& runs, std::string_view value, const text::RunStyle& style, text::Engine& engine) {
	if ((runs.empty() && value.empty()) || (runs.size() == 1 && runs.front().text == value))
		return;
	engine.forget(runs);
	runs.clear();
	add(runs, value, style);
}

text::Layout singleLine(text::Engine& engine, const text::Runs& runs, float width = naturalWidth) {
	return engine.layout(runs, {.width = width, .wrap = text::Wrap::None, .maxLines = 1});
}

std::string language(const tsp::Node& node) {
	std::string result(node.str("lang"));
	if (result.empty()) {
		auto path = node.str("path");
		const auto slash = path.find_last_of("/\\");
		if (slash != std::string_view::npos)
			path.remove_prefix(slash + 1);
		const auto dot = path.find_last_of('.');
		result = dot == std::string_view::npos ? path : path.substr(dot + 1);
	}
	for (auto& c : result)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return result;
}

struct DiffRange {
	int start = 0, count = 1;
};

DiffRange diffRange(std::string_view value) {
	DiffRange range;
	const auto comma = value.find(',');
	const auto number = value.substr(0, comma);
	std::from_chars(number.data(), number.data() + number.size(), range.start);
	if (comma != std::string_view::npos) {
		const auto count = value.substr(comma + 1);
		std::from_chars(count.data(), count.data() + count.size(), range.count);
	}
	return range;
}

void hunkState(std::string_view line, CodeCache::DiffState& state) {
	const auto oldAt = line.find('-');
	const auto newAt = line.find('+', oldAt);
	if (oldAt == std::string_view::npos || newAt == std::string_view::npos)
		return;
	const auto oldRange = diffRange(line.substr(oldAt + 1, newAt - oldAt - 1));
	const auto newRange = diffRange(line.substr(newAt + 1));
	state = {oldRange.start, newRange.start, oldRange.count, newRange.count, true};
}

std::string hunkText(const json& hunks) {
	std::string result;
	for (const auto& hunk : hunks) {
		const auto& lines = hunk.at("lines");
		int oldCount = 0, newCount = 0;
		for (const auto& line : lines) {
			const auto& value = line.get_ref<const std::string&>();
			if (!value.empty()) {
				oldCount += value.front() == '-' || value.front() == ' ';
				newCount += value.front() == '+' || value.front() == ' ';
			}
		}
		result += "@@ -" + std::to_string(hunk.at("oldStart").get<int>()) + "," + std::to_string(oldCount) + " +"
		        + std::to_string(hunk.at("newStart").get<int>()) + "," + std::to_string(newCount) + " @@\n";
		for (const auto& line : lines) {
			result += line.get_ref<const std::string&>();
			result += '\n';
		}
	}
	return result;
}

void number(text::Runs& runs, text::Layout& layout, bool previousHas, int previousValue, bool has, int value,
            text::Engine& engine) {
	if (previousHas == has && previousValue == value)
		return;
	engine.forget(runs);
	runs.clear();
	if (has) {
		auto style = mono(text::palette::dim);
		style.chrome = true;
		runs.push_back({std::to_string(value), style});
	}
	layout = {};
}

void syncLine(CodeCache::Line& line, std::string_view value, bool sameText, bool languageChanged,
              const CodeCache& cache, size_t index, CodeCache::DiffState& state, text::Engine& engine) {
	LineKind kind = LineKind::Code;
	bool hasOld = false, hasNew = !cache.diff && cache.numbers;
	int oldValue = 0, newValue = hasNew ? cache.start + static_cast<int>(index) : 0;
	std::string_view content = value;
	SDL_Color background{0, 0, 0, 0};
	if (cache.diff) {
		if (value.starts_with("@@")) {
			kind = LineKind::Hunk;
			hunkState(value, state);
			background = translucent(text::palette::info, 20);
		} else if (value.starts_with("diff ") || value.starts_with("index ")
		           || (!state.inHunk && (value.starts_with("--- ") || value.starts_with("+++ ")))) {
			kind = LineKind::Meta;
			state.inHunk = false;
		} else if (!value.empty() && (value.front() == '+' || value.front() == '-' || value.front() == ' ')) {
			const char sign = value.front();
			kind = sign == '+' ? LineKind::Insert : sign == '-' ? LineKind::Delete : LineKind::Context;
			content.remove_prefix(1);
			hasOld = sign != '+';
			hasNew = sign != '-';
			if (hasOld)
				oldValue = state.oldNumber++;
			if (hasNew)
				newValue = state.newNumber++;
			if (state.inHunk) {
				state.oldRemaining -= hasOld;
				state.newRemaining -= hasNew;
				if (state.oldRemaining == 0 && state.newRemaining == 0)
					state.inHunk = false;
			}
			if (kind == LineKind::Insert)
				background = translucent(text::palette::success, 35);
			else if (kind == LineKind::Delete)
				background = translucent(text::palette::error, 35);
		} else
			kind = LineKind::Meta;
	}
	if (!sameText || languageChanged || line.kind != kind) {
		engine.forget(line.runs);
		line.runs.clear();
		if (kind == LineKind::Hunk || kind == LineKind::Meta)
			add(line.runs, content, mono(kind == LineKind::Hunk ? text::palette::info : text::palette::muted));
		else
			line.runs = highlightLine(content, cache.language);
		// A real mono run also gives empty lines the mono face's line metrics.
		if (line.runs.empty())
			line.runs.push_back({{}, mono()});
		line.layoutWidth = -1;
		line.previewLines = 0;
	}
	if (line.kind != kind) {
		const std::string_view sign = kind == LineKind::Insert ? "+" : kind == LineKind::Delete ? "-" : "";
		replace(line.sign, sign, mono(kind == LineKind::Insert ? text::palette::success : text::palette::error),
		        engine);
		line.signLayout = {};
	}
	number(line.oldNumber, line.oldNumberLayout, line.hasOld, line.oldValue, hasOld, oldValue, engine);
	number(line.newNumber, line.newNumberLayout, line.hasNew, line.newValue, hasNew, newValue, engine);
	line.kind = kind;
	line.hasOld = hasOld;
	line.hasNew = hasNew;
	line.oldValue = oldValue;
	line.newValue = newValue;
	line.background = background;
	line.after = state;
}

void forgetLine(CodeCache::Line& line, text::Engine& engine) {
	engine.forget(line.runs);
	engine.forget(line.oldNumber);
	engine.forget(line.newNumber);
	engine.forget(line.sign);
}

void previewLayout(CodeCache::Line& line, size_t count) {
	if (count == line.previewLines)
		return;
	line.previewLines = count;
	auto& target = line.previewLayout;
	target.pieces.clear();
	for (const auto& piece : line.layout.pieces) {
		if (static_cast<size_t>(piece.line) >= count)
			break;
		target.pieces.push_back(piece);
	}
	target.lineWidths.assign(line.layout.lineWidths.begin(), line.layout.lineWidths.begin() + count);
	target.width = *std::max_element(target.lineWidths.begin(), target.lineWidths.end());
	target.lineHeight = line.layout.lineHeight;
	target.ascent = line.layout.ascent;
	target.height = static_cast<float>(count) * target.lineHeight;
}

} // namespace

text::Runs highlightLine(std::string_view line, std::string_view language) {
	static const std::unordered_set<std::string_view> keywords = {
	    "if",       "else",     "for",       "while",   "do",         "return", "function", "fn",       "def",
	    "class",    "struct",   "enum",      "const",   "let",        "var",    "mut",      "pub",      "use",
	    "import",   "from",     "export",    "default", "new",        "delete", "async",    "await",    "try",
	    "catch",    "finally",  "throw",     "match",   "case",       "switch", "break",    "continue", "static",
	    "public",   "private",  "protected", "void",    "int",        "bool",   "true",     "false",    "null",
	    "nil",      "None",     "True",      "False",   "self",       "this",   "in",       "of",       "is",
	    "not",      "and",      "or",        "type",    "interface",  "impl",   "trait",    "where",    "namespace",
	    "template", "typename", "auto",      "using",   "package",    "func",   "go",       "defer",    "lambda",
	    "with",     "as",       "yield",     "extends", "implements", "then",   "fi",       "esac",     "done",
	    "echo",     "local",    "readonly"};
	const bool hashComments = language == "bash" || language == "sh" || language == "zsh" || language == "python"
	                       || language == "py" || language == "ruby" || language == "rb" || language == "yaml"
	                       || language == "yml" || language == "toml" || language == "shell" || language == "powershell"
	                       || language == "ps1" || language == "pwsh" || language == "dockerfile" || language == "make"
	                       || language == "makefile";
	const auto plain = mono(), keyword = mono(text::palette::pending), string = mono(text::palette::success),
	           numeric = mono(text::palette::warning), comment = mono(text::palette::dim);
	text::Runs runs;
	size_t plainBegin = 0;
	auto flush = [&](size_t end) {
		add(runs, line.substr(plainBegin, end - plainBegin), plain);
	};
	for (size_t i = 0; i < line.size();) {
		const char c = line[i];
		if ((hashComments && c == '#') || (!hashComments && c == '/' && i + 1 < line.size() && line[i + 1] == '/')
		    || ((language == "sql" || language == "lua") && c == '-' && i + 1 < line.size() && line[i + 1] == '-')) {
			flush(i);
			add(runs, line.substr(i), comment);
			return runs;
		}
		if (c == '"' || c == '\'' || c == '`') {
			size_t end = i + 1;
			while (end < line.size() && line[end] != c)
				end += line[end] == '\\' ? 2 : 1;
			end = std::min(end + 1, line.size());
			flush(i);
			add(runs, line.substr(i, end - i), string);
			i = plainBegin = end;
			continue;
		}
		if (std::isdigit(static_cast<unsigned char>(c))
		    && (i == 0 || !(std::isalnum(static_cast<unsigned char>(line[i - 1])) || line[i - 1] == '_'))) {
			size_t end = i;
			while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '.'))
				++end;
			flush(i);
			add(runs, line.substr(i, end - i), numeric);
			i = plainBegin = end;
			continue;
		}
		if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
			size_t end = i + 1;
			while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
				++end;
			const auto word = line.substr(i, end - i);
			if (keywords.contains(word)) {
				flush(i);
				add(runs, word, keyword);
				plainBegin = end;
			}
			i = end;
			continue;
		}
		++i;
	}
	flush(line.size());
	return runs;
}

void syncCode(const tsp::Node& node, CodeCache& cache, text::Engine& engine, const text::Fonts& fonts,
              bool chromeless) {
	const bool diff = node.kind == tsp::Kind::Diff;
	const bool fontChanged = cache.fontSize != fonts.size();
	if (cache.synced && cache.revision == node.rev && cache.chromeless == chromeless && cache.diff == diff
	    && !fontChanged)
		return;
	const auto nextLanguage = language(node);
	const bool languageChanged = !cache.synced || cache.language != nextLanguage || cache.diff != diff;
	const int start = static_cast<int>(node.num("start", 1));
	const bool numbers = !diff && node.flag("numbers"), wrap = diff || node.flag("wrap", true);
	const bool reparse = languageChanged || cache.start != start || cache.numbers != numbers;
	const bool wrapChanged = cache.wrap != wrap;
	cache.diff = diff;
	cache.language = nextLanguage;
	cache.start = start;
	cache.numbers = numbers;
	cache.wrap = wrap;

	std::string structured;
	auto source = node.str("text");
	if (diff && source.empty() && node.prop("hunks").is_array()) {
		structured = hunkText(node.prop("hunks"));
		source = structured;
	}
	const bool sourceChanged = source != cache.source;
	if (sourceChanged || reparse) {
		size_t at = 0, index = 0;
		CodeCache::DiffState state;
		if (!reparse && source.starts_with(cache.source)) {
			at = cache.tailByte;
			index = cache.tailLine;
			if (index > 0)
				state = cache.lines[index - 1].after;
		}
		while (at < source.size() || (!diff && source.empty() && index == 0)) {
			const auto newline = source.find('\n', at);
			const auto end = newline == std::string_view::npos ? source.size() : newline;
			const auto contentEnd = end > at && source[end - 1] == '\r' ? end - 1 : end;
			const auto value = source.substr(at, contentEnd - at);
			const bool existed = index < cache.lines.size();
			if (!existed)
				cache.lines.emplace_back();
			auto& line = cache.lines[index];
			const bool sameText =
			    existed && std::string_view(cache.source).substr(line.begin, line.end - line.begin) == value;
			syncLine(line, value, sameText, languageChanged, cache, index, state, engine);
			line.begin = at;
			line.end = contentEnd;
			++index;
			if (newline == std::string_view::npos)
				break;
			at = newline + 1;
		}
		for (size_t i = index; i < cache.lines.size(); ++i)
			forgetLine(cache.lines[i], engine);
		cache.lines.resize(index);
		const auto lastNewline = source.find_last_of('\n');
		cache.tailByte = lastNewline == std::string_view::npos ? 0 : lastNewline + 1;
		cache.tailLine = source.empty()                  ? 0
		               : cache.tailByte == source.size() ? cache.lines.size()
		                                                 : cache.lines.size() - 1;
		if (sourceChanged) {
			if (source.starts_with(cache.source))
				cache.source.append(source.substr(cache.source.size()));
			else
				cache.source = source;
		}
	}

	cache.oldColumn = cache.newColumn = 0;
	for (auto& line : cache.lines) {
		if (fontChanged || wrapChanged) {
			line.layoutWidth = -1;
			line.previewLines = 0;
		}
		if (line.hasOld && (fontChanged || line.oldNumberLayout.lineHeight == 0))
			line.oldNumberLayout = singleLine(engine, line.oldNumber);
		if (line.hasNew && (fontChanged || line.newNumberLayout.lineHeight == 0))
			line.newNumberLayout = singleLine(engine, line.newNumber);
		if (!line.sign.empty() && (fontChanged || line.signLayout.lineHeight == 0))
			line.signLayout = singleLine(engine, line.sign);
		cache.oldColumn = std::max(cache.oldColumn, line.oldNumberLayout.width);
		cache.newColumn = std::max(cache.newColumn, line.newNumberLayout.width);
		if (!diff)
			line.background = {0, 0, 0, 0};
	}
	if (!diff && node.prop("marks").is_array()) {
		for (const auto& mark : node.prop("marks")) {
			const int index = mark.at("line").get<int>() - start;
			if (index < 0 || static_cast<size_t>(index) >= cache.lines.size())
				continue;
			auto style = mono();
			text::applyTokens(mark.at("tone").get_ref<const std::string&>(), style);
			cache.lines[static_cast<size_t>(index)].background = style.bg.a ? style.bg : translucent(style.color, 35);
		}
	}
	replace(cache.header, !diff && !chromeless ? node.str("path") : std::string_view{}, mono(text::palette::muted),
	        engine);
	const auto& preview = node.prop("preview");
	cache.preview = !diff && !preview.is_null() ? (preview.is_object() ? preview.value("lines", 12) : 12) : 0;
	cache.fontSize = fonts.size();
	cache.chromeless = chromeless;
	cache.revision = node.rev;
	cache.synced = true;
	cache.dirty = true;
}

SDL_FPoint measureCode(const tsp::Node& node, CodeCache& cache, text::Engine& engine, const text::Fonts& fonts,
                       float width, bool expanded) {
	syncCode(node, cache, engine, fonts, cache.chromeless);
	width = std::max(0.0f, width);
	if (!cache.dirty && cache.width == width && cache.expanded == expanded)
		return {cache.measuredWidth, cache.height};
	cache.width = width;
	cache.expanded = expanded;
	cache.padX = fonts.size() * gfx::space::lg;
	cache.padY = fonts.size() * gfx::space::lg;
	const float gap = fonts.size() * gfx::space::sm;
	if (cache.diff) {
		const float minimum = fonts.monoAdvance() * 2;
		cache.oldColumn = std::max(minimum, cache.oldColumn);
		cache.newColumn = std::max(minimum, cache.newColumn);
		cache.signColumn = minimum;
		cache.gutter = cache.oldColumn + cache.newColumn + cache.signColumn + gap * 2;
	} else {
		cache.signColumn = 0;
		cache.gutter = cache.numbers ? cache.newColumn + gap : 0;
	}
	cache.headerHeight = 0;
	if (!cache.header.empty()) {
		cache.headerLayout = singleLine(engine, cache.header, std::max(0.0f, width - cache.padX * 2));
		cache.headerHeight = cache.headerLayout.height + gap;
	}
	const bool clamp = !cache.diff && !expanded && cache.preview > 0;
	size_t remaining = clamp ? static_cast<size_t>(cache.preview) : std::numeric_limits<size_t>::max();
	size_t total = 0, visible = 0;
	cache.visibleLines = 0;
	cache.contentHeight = 0;
	float intrinsicWidth = cache.header.empty() ? 0 : cache.headerLayout.width + cache.padX * 2;
	for (auto& line : cache.lines) {
		const bool fullWidth = line.kind == LineKind::Hunk || line.kind == LineKind::Meta;
		const float available = std::max(0.0f, width - cache.padX * 2 - (fullWidth ? 0 : cache.gutter));
		if (line.layoutWidth != available) {
			line.layout = engine.layout(line.runs,
			                            {.width = available, .wrap = cache.wrap ? text::Wrap::Char : text::Wrap::None});
			line.layoutWidth = available;
			line.previewLines = 0;
		}
		intrinsicWidth = std::max(intrinsicWidth, line.layout.width + cache.padX * 2 + (fullWidth ? 0 : cache.gutter));
		const size_t count = line.layout.lineWidths.size(), shown = std::min(count, remaining);
		line.y = cache.padY + cache.contentHeight;
		line.visibleHeight = static_cast<float>(shown) * line.layout.lineHeight;
		if (shown > 0) {
			++cache.visibleLines;
			cache.contentHeight += line.visibleHeight;
			if (shown < count)
				previewLayout(line, shown);
		}
		remaining -= shown;
		total += count;
		visible += shown;
	}
	const size_t hidden = total - visible;
	if (cache.hiddenLines != hidden) {
		if (hidden > 0)
			replace(cache.more, std::to_string(hidden) + " more lines", mono(text::palette::accent), engine);
		else
			replace(cache.more, {}, mono(text::palette::accent), engine);
		cache.hiddenLines = hidden;
	}
	cache.bodyHeight = cache.padY * 2 + cache.contentHeight;
	cache.moreHeight = 0;
	if (!cache.more.empty()) {
		cache.moreLayout = singleLine(engine, cache.more, std::max(0.0f, width - cache.padX * 2));
		cache.moreHeight = gap + cache.moreLayout.height;
	}
	cache.height = cache.headerHeight + cache.bodyHeight + cache.moreHeight;
	cache.measuredWidth = width >= naturalWidth
	                        ? std::max(intrinsicWidth, cache.more.empty() ? 0 : cache.moreLayout.width + cache.padX * 2)
	                        : width;
	cache.dirty = false;
	return {cache.measuredWidth, cache.height};
}

bool paintCode(const tsp::Node& node, CodeCache& cache, text::Engine& engine, const text::Fonts& fonts,
               SDL_Renderer& renderer, const SDL_FRect& rect, bool expanded, view::HitTargets& targets) {
	measureCode(node, cache, engine, fonts, rect.w, expanded);
	gfx::ScopedClip clip(renderer, rect);
	const float gap = fonts.size() * gfx::space::sm;
	if (!cache.header.empty())
		engine.draw(cache.header, cache.headerLayout, rect.x + cache.padX, rect.y);
	const SDL_FRect body{rect.x, rect.y + cache.headerHeight, rect.w, cache.bodyHeight};
	gfx::rounded(renderer, body, fonts.size() / 3, text::palette::surface2);
	{
		gfx::ScopedClip contentClip(renderer, {body.x, body.y + cache.padY, body.w, cache.contentHeight});
		for (size_t i = 0; i < cache.visibleLines; ++i) {
			const auto& line = cache.lines[i];
			const float y = body.y + line.y;
			if (!gfx::intersects({body.x, y, body.w, line.visibleHeight}, contentClip.visible))
				continue;
			if (line.background.a)
				gfx::fill(renderer, {body.x, y, body.w, line.visibleHeight}, line.background);
			const float x = body.x + cache.padX;
			if (cache.diff) {
				if (line.hasOld)
					engine.draw(line.oldNumber, line.oldNumberLayout, x + cache.oldColumn - line.oldNumberLayout.width,
					            y);
				if (line.hasNew)
					engine.draw(line.newNumber, line.newNumberLayout,
					            x + cache.oldColumn + gap + cache.newColumn - line.newNumberLayout.width, y);
				if (!line.sign.empty())
					engine.draw(line.sign, line.signLayout, x + cache.oldColumn + cache.newColumn + gap * 2, y);
			} else if (line.hasNew)
				engine.draw(line.newNumber, line.newNumberLayout, x + cache.newColumn - line.newNumberLayout.width, y);
			const bool fullWidth = line.kind == LineKind::Hunk || line.kind == LineKind::Meta;
			const auto& layout = line.visibleHeight < line.layout.height ? line.previewLayout : line.layout;
			engine.draw(line.runs, layout, x + (fullWidth ? 0 : cache.gutter), y);
		}
	}
	if (!cache.more.empty()) {
		const float y = body.y + body.h + gap;
		engine.draw(cache.more, cache.moreLayout, rect.x + cache.padX, y);
		targets.add(gfx::intersect({rect.x, y, rect.w, cache.moreLayout.height}, clip.visible),
		            view::TargetKind::Expand);
	}
	return false;
}

void forgetCode(CodeCache& cache, text::Engine& engine) {
	for (auto& line : cache.lines)
		forgetLine(line, engine);
	engine.forget(cache.header);
	engine.forget(cache.more);
	cache = {};
}

} // namespace kinds
