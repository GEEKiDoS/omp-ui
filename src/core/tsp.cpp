#include "tsp.h"

#include "text_util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace tsp {

namespace {

constexpr std::array<std::string_view, 44> kindNames = {
    "col",     "row",     "card",     "section", "rule",  "spacer", "text",      "md",     "code",  "diff",   "ansi",
    "math",    "image",   "media",    "kv",      "table", "tree",   "json",      "badge",  "kbd",   "icon",   "spinner",
    "shimmer", "elapsed", "progress", "rate",    "list",  "item",   "tabs",      "editor", "input", "status", "seg",
    "overlay", "toast",   "rows",     "picker",  "prefs", "tool",   "checklist", "agent",  "chart", "meter",  "effort"};

struct OpError {
	std::string msg;
};

[[noreturn]] void fail(std::string msg) {
	throw OpError{std::move(msg)};
}

bool isKeyByte(unsigned char b) {
	return (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '_' || b == '-';
}

/** `key=value` where key is `[A-Za-z0-9_-]+` and value printable ASCII without `;`. */
bool isParam(std::string_view seg) {
	size_t eq = seg.find('=');
	if (eq == 0 || eq == std::string_view::npos)
		return false;
	for (size_t i = 0; i < eq; i++)
		if (!isKeyByte(static_cast<unsigned char>(seg[i])))
			return false;
	for (size_t i = eq + 1; i < seg.size(); i++) {
		auto b = static_cast<unsigned char>(seg[i]);
		if (b < 0x21 || b > 0x7e || b == ';')
			return false;
	}
	return true;
}

/** Whether `body` from `at` leads with a `key=value;` segment a receiver would read as a parameter. */
bool leadsWithParameter(std::string_view body, size_t at) {
	size_t i = at;
	while (i < body.size() && isKeyByte(static_cast<unsigned char>(body[i])))
		i++;
	if (i == at || i >= body.size() || body[i] != '=')
		return false;
	i++;
	while (i < body.size() && static_cast<unsigned char>(body[i]) >= 0x21 && static_cast<unsigned char>(body[i]) <= 0x7e
	       && body[i] != ';')
		i++;
	return i < body.size() && body[i] == ';';
}

/** Safe chunk end nearest below `start + limit`, else above it, else the end (omp's `chunkEnd`). */
size_t chunkEnd(std::string_view body, size_t start, size_t limit) {
	size_t target = start + limit;
	if (target >= body.size())
		return body.size();
	auto safe = [&](size_t at) {
		return (static_cast<unsigned char>(body[at]) & 0xc0) != 0x80 && !leadsWithParameter(body, at);
	};
	for (size_t at = target; at > start; at--)
		if (safe(at))
			return at;
	for (size_t at = target + 1; at < body.size(); at++)
		if (safe(at))
			return at;
	return body.size();
}

std::string truncateForLog(const std::string& s, size_t max = 240) {
	if (s.size() <= max)
		return s;
	size_t cut = max;
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xc0) == 0x80)
		cut--;
	return s.substr(0, cut) + "… (" + std::to_string(s.size()) + " bytes)";
}

} // namespace

std::optional<Kind> parseKind(std::string_view name) {
	auto it = std::find(kindNames.begin(), kindNames.end(), name);
	if (it == kindNames.end())
		return std::nullopt;
	return static_cast<Kind>(it - kindNames.begin());
}

std::string_view kindName(Kind kind) {
	return kindNames[static_cast<size_t>(kind)];
}

bool isTextKind(Kind kind) {
	switch (kind) {
	case Kind::Text:
	case Kind::Md:
	case Kind::Code:
	case Kind::Ansi:
	case Kind::Math:
	case Kind::Editor:
	case Kind::Input:
	case Kind::Shimmer:
		return true;
	default:
		return false;
	}
}

double nowMs() {
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::optional<RawMessage> splitMessage(std::string_view apc) {
	if (apc.substr(0, 4) != "tsp;")
		return std::nullopt;
	std::string_view inner = apc.substr(4);
	size_t semi = inner.find(';');
	if (semi == std::string_view::npos) {
		if (inner.empty())
			return std::nullopt;
		return RawMessage{std::string(inner), {}, ""};
	}
	if (semi == 0)
		return std::nullopt;
	RawMessage msg;
	msg.verb = std::string(inner.substr(0, semi));
	size_t pos = semi + 1;
	// A segment is a parameter only when another `;` follows it; the body is the rest.
	for (;;) {
		semi = inner.find(';', pos);
		if (semi == std::string_view::npos)
			break;
		std::string_view seg = inner.substr(pos, semi - pos);
		if (!isParam(seg))
			break;
		size_t eq = seg.find('=');
		msg.params[std::string(seg.substr(0, eq))] = std::string(seg.substr(eq + 1));
		pos = semi + 1;
	}
	msg.body = std::string(inner.substr(pos));
	return msg;
}

std::string encodeReply(char verb, const std::string& body, size_t limit) {
	auto frame = [&](std::string_view params, std::string_view piece) {
#ifdef _WIN32
		std::string out = "\x1b]877;tsp;";
#else
		std::string out = "\x1b_tsp;";
#endif
		out.push_back(verb);
		out.append(params);
		out.push_back(';');
		out.append(piece);
		out.append("\x1b\\");
		return out;
	};
	if (body.size() <= limit)
		return frame("", body);
	static uint64_t nextChunk = 1;
	std::string id = std::to_string(nextChunk++);
	std::string out;
	for (size_t start = 0; start < body.size();) {
		size_t end = chunkEnd(body, start, std::max<size_t>(1, limit));
		std::string params = ";c=" + id + (end < body.size() ? ";m=1" : "");
		out += frame(params, std::string_view(body).substr(start, end - start));
		start = end;
	}
	return out;
}

// ─── Node ────────────────────────────────────────────────────────────────

const json& Node::prop(std::string_view key) const {
	static const json null;
	auto it = p.find(key);
	return it == p.end() ? null : *it;
}

std::string_view Node::str(std::string_view key, std::string_view fallback) const {
	const json& value = prop(key);
	return value.is_string() ? std::string_view(value.get_ref<const std::string&>()) : fallback;
}

double Node::num(std::string_view key, double fallback) const {
	const json& value = prop(key);
	return value.is_number() ? value.get<double>() : fallback;
}

bool Node::flag(std::string_view key, bool fallback) const {
	const json& value = prop(key);
	return value.is_boolean() ? value.get<bool>() : fallback;
}

double Node::since(std::string_view key) const {
	auto it = stamps.find(std::string(key));
	return it == stamps.end() ? 0 : nowMs() - it->second;
}

// ─── Document ────────────────────────────────────────────────────────────

Document::Document(std::string surface) {
	nodes_.emplace_back(); // Slot zero is never used.
	root_ = allocate();
	at(root_).id = std::move(surface);
	index_.emplace(at(root_).id, root_);
}

Document::~Document() {
	teardown();
	detach();
}

NodeId Document::allocate() {
	if (free_.empty()) {
		nodes_.emplace_back();
		return static_cast<NodeId>(nodes_.size() - 1);
	}
	NodeId id = free_.back();
	free_.pop_back();
	return id;
}

std::optional<NodeId> Document::find(std::string_view id) const {
	auto it = index_.find(std::string(id));
	return it == index_.end() ? std::nullopt : std::optional(it->second);
}

NodeId Document::node(const std::string& id) const {
	auto found = index_.find(id);
	if (found == index_.end())
		fail("unknown id " + id);
	return found->second;
}

NodeId Document::textNode(const std::string& id) const {
	NodeId handle = node(id);
	if (!isTextKind(at(handle).kind))
		fail(id + " (" + std::string(kindName(at(handle).kind)) + ") has no primary text");
	return handle;
}

void Document::changeFocus(std::optional<NodeId> focus) {
	focus_ = focus;
	if (hooks_.focusChanged)
		hooks_.focusChanged(focus_);
}

void Document::removeSubtree(NodeId id) {
	if (focus_) {
		for (NodeId cursor = *focus_; cursor; cursor = at(cursor).parent) {
			if (cursor == id) {
				changeFocus(std::nullopt);
				break;
			}
		}
	}
	// The callback sees the original parent, sibling order and complete id index.
	if (hooks_.removing)
		hooks_.removing(id);
	auto& siblings = at(at(id).parent).children;
	siblings.erase(std::find(siblings.begin(), siblings.end(), id));
	unregister(id);
}

void Document::close() {
	changeFocus(std::nullopt);
	for (std::string_view region : {"dock", "layer"}) {
		auto id = find(region);
		if (id && at(*id).parent == root_)
			removeSubtree(*id);
	}
	version_++;
}

void Document::teardown() {
	if (focus_)
		changeFocus(std::nullopt);
	while (!at(root_).children.empty())
		removeSubtree(at(root_).children.front());
}

std::vector<ApplyError> Document::applyFrame(int64_t s, const json& ops) {
	std::vector<ApplyError> errors;
	if (!ops.is_array()) {
		errors.push_back({s, -1, "ops is not an array"});
		return errors;
	}
	for (size_t i = 0; i < ops.size(); i++) {
		try {
			apply(ops[i]);
		} catch (const OpError& e) {
			errors.push_back({s, static_cast<int>(i), e.msg});
		} catch (const json::exception& e) {
			errors.push_back({s, static_cast<int>(i), std::string("malformed op: ") + e.what()});
		}
	}
	version_++;
	return errors;
}

size_t Document::insertIndex(NodeId parent, const json& before) const {
	const auto& children = at(parent).children;
	if (before.is_null())
		return children.size();
	auto sibling = find(before.get<std::string>());
	if (!sibling || at(*sibling).parent != parent)
		fail(before.get<std::string>() + " is not a child of " + at(parent).id);
	return std::find(children.begin(), children.end(), *sibling) - children.begin();
}

void Document::mergeProps(Node& n, const json& props, bool fromSet) {
	const double now = nowMs();
	for (auto it = props.begin(); it != props.end(); ++it) {
		if (it.value().is_null()) {
			if (fromSet)
				n.p.erase(it.key());
		} else {
			n.p[it.key()] = it.value();
		}
		n.stamps[it.key()] = now;
	}
	// A picker's catalogue is sent once and patched by id.
	if (n.kind == Kind::Picker) {
		auto itemId = [](const json& item) -> const json& {
			static const json absent = "";
			auto id = item.find("id");
			return id == item.end() ? absent : *id;
		};
		// Elapsed picker facts retain their own receipt time across catalogue patches.
		if (props.contains("items")) {
			for (auto it = n.stamps.begin(); it != n.stamps.end();) {
				if (it->first.starts_with("items/"))
					it = n.stamps.erase(it);
				else
					++it;
			}
			if (n.prop("items").is_array())
				for (const auto& item : n.prop("items"))
					if (itemId(item).is_string())
						n.stamps["items/" + itemId(item).get_ref<const std::string&>()] = now;
		}
		if (auto add = n.p.find("itemsAdd"); add != n.p.end()) {
			json& items = n.p["items"];
			if (!items.is_array())
				items = json::array();
			for (const json& item : *add) {
				auto same = std::find_if(items.begin(), items.end(),
				                         [&](const json& have) { return itemId(have) == itemId(item); });
				if (same != items.end())
					*same = item;
				else
					items.push_back(item);
				if (itemId(item).is_string())
					n.stamps["items/" + itemId(item).get_ref<const std::string&>()] = now;
			}
			n.p.erase("itemsAdd");
		}
		if (auto del = n.p.find("itemsDel"); del != n.p.end()) {
			if (auto items = n.p.find("items"); items != n.p.end() && items->is_array()) {
				std::unordered_set<std::string> gone;
				for (const json& id : *del)
					if (id.is_string()) {
						gone.insert(id.get<std::string>());
						n.stamps.erase("items/" + id.get_ref<const std::string&>());
					}
				json kept = json::array();
				for (const json& item : *items) {
					const json& id = itemId(item);
					if (!id.is_string() || !gone.count(id.get_ref<const std::string&>()))
						kept.push_back(item);
				}
				*items = std::move(kept);
			}
			n.p.erase("itemsDel");
		}
	}
	n.rev++;
}

NodeId Document::build(const json& wire, NodeId parent, std::unordered_set<std::string>& seen,
                       std::vector<NodeId>& built) {
	if (!wire.is_object())
		fail("node is not an object");
	auto id = wire.find("id");
	if (id == wire.end() || !id->is_string() || id->get_ref<const std::string&>().empty())
		fail("node without id");
	auto k = wire.find("k");
	auto kind = k != wire.end() && k->is_string() ? parseKind(k->get_ref<const std::string&>()) : std::nullopt;
	if (!kind)
		fail("unknown kind "
		     + (k == wire.end() ? std::string("undefined")
		                        : (k->is_string() ? k->get_ref<const std::string&>() : k->dump())));
	const std::string& wireId = id->get_ref<const std::string&>();
	if (index_.count(wireId) || !seen.insert(wireId).second)
		fail("duplicate id " + wireId);
	NodeId handle = allocate();
	built.push_back(handle);
	Node& n = at(handle);
	n.id = wireId;
	n.kind = *kind;
	n.parent = parent;
	if (auto p = wire.find("p"); p != wire.end() && p->is_object())
		mergeProps(n, *p, false);
	if (auto c = wire.find("c"); c != wire.end() && c->is_array())
		for (const json& child : *c)
			n.children.push_back(build(child, handle, seen, built));
	return handle;
}

void Document::unregister(NodeId id) {
	Node& n = at(id);
	for (NodeId child : n.children)
		unregister(child);
	index_.erase(n.id);
	n = Node{};
	free_.push_back(id);
}

void Document::apply(const json& op) {
	if (!op.is_array() || op.empty() || !op[0].is_string())
		fail("op is not a tagged array");
	const std::string& name = op[0].get_ref<const std::string&>();
	auto arg = [&](size_t i) -> const json& {
		if (i >= op.size())
			fail(name + " is missing argument " + std::to_string(i));
		return op[i];
	};
	auto strArg = [&](size_t i) -> const std::string& {
		const json& v = arg(i);
		if (!v.is_string())
			fail(name + " argument " + std::to_string(i) + " must be a string");
		return v.get_ref<const std::string&>();
	};

	if (name == "add") {
		const std::string& id = strArg(1);
		const json& wire = arg(4);
		if (!wire.is_object() || wire.value("id", "") != id)
			fail("add id " + id + " does not match node id "
			     + (wire.is_object() && wire.contains("id")
			            ? (wire["id"].is_string() ? wire["id"].get_ref<const std::string&>() : wire["id"].dump())
			            : "undefined"));
		NodeId parent = node(strArg(2));
		size_t position = insertIndex(parent, arg(3));
		std::unordered_set<std::string> seen;
		std::vector<NodeId> built;
		NodeId inserted;
		try {
			inserted = build(wire, parent, seen, built);
		} catch (...) {
			// A rejected subtree is never visible and must not consume document slots.
			for (NodeId handle : built) {
				at(handle) = Node{};
				free_.push_back(handle);
			}
			throw;
		}
		for (NodeId handle : built)
			index_.emplace(at(handle).id, handle);
		auto& children = at(parent).children;
		children.insert(children.begin() + static_cast<ptrdiff_t>(position), inserted);
		if (hooks_.added)
			hooks_.added(inserted);
	} else if (name == "set") {
		NodeId id = node(strArg(1));
		const json& props = arg(2);
		if (!props.is_object())
			fail("set props must be an object");
		mergeProps(at(id), props, true);
		if (hooks_.propsChanged)
			hooks_.propsChanged(id, props);
	} else if (name == "text") {
		NodeId id = textNode(strArg(1));
		Node& n = at(id);
		const std::string& mode = strArg(2);
		if (!arg(3).is_string())
			fail("text must be a string");
		const std::string& replacement = arg(3).get_ref<const std::string&>();
		size_t from = 0;
		if (mode == "append") {
			from = n.str("text").size();
			if (!n.prop("text").is_string())
				n.p["text"] = "";
			n.p["text"].get_ref<std::string&>().append(replacement);
		} else if (mode == "replace") {
			auto previous = n.str("text");
			while (from < previous.size() && from < replacement.size() && previous[from] == replacement[from])
				from++;
			n.p["text"] = replacement;
		} else {
			fail("unknown text mode " + mode);
		}
		n.stamps["text"] = nowMs();
		n.rev++;
		if (hooks_.textChanged)
			hooks_.textChanged(id, from);
	} else if (name == "splice") {
		NodeId id = textNode(strArg(1));
		Node& n = at(id);
		const json& offset = arg(2);
		const json& count = arg(3);
		const std::string& replacement = strArg(4);
		std::string_view current = n.str("text");
		size_t length = text::utf16Length(current);
		auto validExtent = [length](const json& value) {
			if (!value.is_number())
				return false;
			double extent = value.get<double>();
			return extent >= 0 && extent <= static_cast<double>(length) && std::floor(extent) == extent;
		};
		if (!validExtent(offset) || !validExtent(count) || count.get<double>() > length - offset.get<double>())
			fail("splice range " + offset.dump() + "+" + count.dump() + " outside text of length "
			     + std::to_string(length));
		size_t from = text::byteOffset(current, offset.get<size_t>());
		size_t to = text::byteOffset(current, offset.get<size_t>() + count.get<size_t>());
		if (!n.prop("text").is_string())
			n.p["text"] = "";
		n.p["text"].get_ref<std::string&>().replace(from, to - from, replacement);
		n.stamps["text"] = nowMs();
		n.rev++;
		if (hooks_.textChanged)
			hooks_.textChanged(id, from);
	} else if (name == "move") {
		NodeId id = node(strArg(1));
		if (id == root_)
			fail("cannot move the root");
		NodeId parent = node(strArg(2));
		for (NodeId cursor = parent; cursor; cursor = at(cursor).parent)
			if (cursor == id)
				fail("cannot move " + at(id).id + " into its own subtree");
		const json& before = arg(3);
		if (before.is_string() && before.get_ref<const std::string&>() == at(id).id)
			fail("move before itself");
		insertIndex(parent, before); // Validate before detaching, so rejection leaves the tree intact.
		auto& old = at(at(id).parent).children;
		old.erase(std::find(old.begin(), old.end(), id));
		auto& children = at(parent).children;
		children.insert(children.begin() + static_cast<ptrdiff_t>(insertIndex(parent, before)), id);
		at(id).parent = parent;
		if (hooks_.moved)
			hooks_.moved(id);
	} else if (name == "del") {
		NodeId id = node(strArg(1));
		if (id == root_)
			fail("cannot delete the root");
		removeSubtree(id);
	} else if (name == "settle") {
		node(strArg(1)); // A hint; later ops remain unrestricted.
	} else if (name == "focus") {
		if (arg(1).is_null())
			changeFocus(std::nullopt);
		else
			changeFocus(node(strArg(1)));
	} else if (name == "reveal") {
		node(strArg(1));
		reveals.emplace_back(strArg(1), op.size() > 2 && op[2].is_string() ? op[2].get<std::string>() : "nearest");
	} else if (name == "scroll") {
		node(strArg(1));
		scrolls.emplace_back(strArg(1), strArg(2));
	} else if (name == "suspend") {
		suspended_ = true;
	} else if (name == "resume") {
		suspended_ = false;
	} else {
		fail("unknown op " + name);
	}
}

json Document::exportNode(NodeId id) const {
	const Node& n = at(id);
	json out = {{"id", n.id}, {"k", kindName(n.kind)}};
	if (!n.p.empty())
		out["p"] = n.p;
	if (!n.children.empty()) {
		json children = json::array();
		for (NodeId child : n.children)
			children.push_back(exportNode(child));
		out["c"] = std::move(children);
	}
	return out;
}

json Document::snapshot() const {
	return exportNode(root_);
}

// ─── Session ─────────────────────────────────────────────────────────────

void Session::record(bool out, const std::string& verb, const std::string& body) {
	log.push_back({out, verb, body.size(), truncateForLog(body)});
	if (onLog)
		onLog(log.back());
	while (log.size() > logLimit)
		log.pop_front();
}

void Session::reply(const json& body) {
	std::string text = body.dump();
	record(true, "r", text);
	write_(encodeReply('r', text));
}

void Session::sendEvent(const json& event) {
	std::string text = event.dump();
	record(true, "e", text);
	write_(encodeReply('e', text));
}

bool Session::programFeature(const std::string& name) const {
	return std::find(appFeatures_.begin(), appFeatures_.end(), name) != appFeatures_.end();
}

Session::~Session() {
	teardownAll();
}

void Session::closing(Surface& surface) {
	surface.doc->teardown();
	if (surfaceClosing)
		surfaceClosing(surface);
	surface.doc->detach();
}

void Session::teardownAll() {
	for (auto& surface : surfaces_)
		closing(*surface);
	surfaces_.clear();
	pendingAcks_.clear();
}

std::optional<std::reference_wrapper<Surface>> Session::find(std::string_view id) {
	for (auto& surface : surfaces_)
		if (surface->id == id)
			return *surface;
	return std::nullopt;
}

std::optional<std::reference_wrapper<Surface>> Session::screen() {
	for (auto it = surfaces_.rbegin(); it != surfaces_.rend(); ++it)
		if ((*it)->open && (*it)->mode == "screen")
			return **it;
	return std::nullopt;
}

void Session::resize(int newCols, float newCellW, float newCellH) {
	if (newCols == cols && newCellW == cellW && newCellH == cellH)
		return;
	cols = newCols;
	cellW = newCellW;
	cellH = newCellH;
	if (helloAnswered_)
		sendEvent({{"ev", "resize"}, {"cols", cols}, {"cell", {{"w", cellW}, {"h", cellH}}}});
}

void Session::flushAcks() {
	for (auto& [sf, s] : pendingAcks_)
		sendEvent({{"ev", "ack"}, {"sf", sf}, {"s", s}});
	pendingAcks_.clear();
}

bool Session::onApc(std::string_view body) {
	auto raw = splitMessage(body);
	if (!raw)
		return false;
	if (auto c = raw->params.find("c"); c != raw->params.end()) {
		const std::string chunk = c->second;
		auto& joined = chunks_[chunk];
		if (joined.body.empty() && joined.params.empty())
			joined.params = raw->params; // The first chunk names the message.
		joined.body += raw->body;
		auto more = raw->params.find("m");
		if (more != raw->params.end() && more->second == "1")
			return true;
		raw->body = std::move(joined.body);
		raw->params = std::move(joined.params);
		chunks_.erase(chunk);
	}
	try {
		dispatch(*raw);
	} catch (const json::exception& e) {
		lastError = "malformed " + raw->verb + " message: " + e.what();
	}
	version++;
	return true;
}

std::optional<std::string_view> Session::blob(std::string_view id) const {
	const auto found = blobs_.find(std::string(id));
	if (found == blobs_.end())
		return std::nullopt;
	return found->second;
}

void Session::dispatch(const RawMessage& msg) {
	if (msg.verb == "b") {
		// Blob chunk: base64 bytes named by the sender's content address.
		record(false, "b",
		       "blob " + (msg.params.count("id") ? msg.params.at("id") : std::string("?")) + " ("
		           + std::to_string(msg.body.size()) + " base64 bytes)");
		if (auto id = msg.params.find("id"); id != msg.params.end()) {
			blobs_[id->second] = msg.body;
			if (blobAdded)
				blobAdded(id->second);
		}
		return;
	}
	record(false, msg.verb, msg.body);
	if (msg.verb == "t")
		return; // Palette: this renderer keeps its own theme.
	const json body = json::parse(msg.body);

	if (msg.verb == "q") {
		const std::string q = body.value("q", "");
		if (q == "hello") {
			app_ = body.value("app", "");
			appVersion_ = body.value("ver", "");
			appFeatures_.clear();
			if (auto f = body.find("features"); f != body.end() && f->is_array())
				for (const json& name : *f)
					if (name.is_string())
						appFeatures_.push_back(name.get<std::string>());
			bool speaks = false;
			if (auto v = body.find("v"); v != body.end() && v->is_array())
				for (const json& ver : *v)
					speaks = speaks || (ver.is_number_integer() && ver.get<int>() == kVersion);
			if (!speaks) {
				lastError = "program does not speak TSP v1";
				return; // No reply: its DA1 sentinel resolves the probe as unsupported.
			}
			reply({{"r", "hello"},
			       {"v", kVersion},
			       {"term", term},
			       {"ver", termVersion},
			       {"kinds", kinds},
			       {"features", features},
			       {"apc", kApcLimit},
			       {"credits", credits},
			       {"cols", cols},
			       {"cell", {{"w", cellW}, {"h", cellH}}},
			       {"dark", dark},
			       {"reduceMotion", false}});
			helloAnswered_ = true;
		} else if (q == "blobs") {
			json have = json::array();
			for (const json& id : body.value("ids", json::array()))
				if (id.is_string() && blobs_.count(id.get<std::string>()))
					have.push_back(id);
			reply({{"r", "blobs"}, {"have", have}});
		}
		return;
	}

	if (msg.verb == "o") {
		const std::string id = body.at("id").get<std::string>();
		auto existing = find(id);
		if (body.value("adopt", false)) {
			if (!existing) {
				sendEvent({{"ev", "gone"}, {"ids", json::array({id})}});
				return;
			}
			Surface& surface = existing->get();
			surface.open = true;
			auto it = std::find_if(surfaces_.begin(), surfaces_.end(), [&](auto& s) { return s->id == id; });
			std::rotate(it, it + 1, surfaces_.end());
			if (surfaceChanged)
				surfaceChanged(surface);
			return;
		}
		if (existing) {
			closing(existing->get());
			surfaces_.erase(std::find_if(surfaces_.begin(), surfaces_.end(), [&](auto& s) { return s->id == id; }));
			pendingAcks_.erase(id);
		}
		auto surface = std::make_unique<Surface>();
		surface->id = id;
		surface->mode = body.value("mode", "inline");
		surface->title = body.value("title", "");
		surface->role = body.value("role", "");
		surface->doc = std::make_unique<Document>(id);
		surfaces_.push_back(std::move(surface));
		if (surfaceOpened)
			surfaceOpened(*surfaces_.back());
		return;
	}

	if (msg.verb == "x") {
		const std::string id = body.at("id").get<std::string>();
		auto found = find(id);
		if (!found)
			return;
		Surface& surface = found->get();
		if (body.value("keep", false) && surface.mode == "inline") {
			surface.doc->close();
			surface.open = false;
			if (surfaceChanged)
				surfaceChanged(surface);
		} else {
			closing(surface);
			surfaces_.erase(std::find_if(surfaces_.begin(), surfaces_.end(), [&](auto& s) { return s->id == id; }));
			pendingAcks_.erase(id);
		}
		return;
	}

	if (msg.verb == "f") {
		const std::string sf = body.at("sf").get<std::string>();
		const int64_t seq = body.at("s").get<int64_t>();
		auto found = find(sf);
		if (!found) {
			sendEvent({{"ev", "error"}, {"sf", sf}, {"s", seq}, {"msg", "frame for unknown surface"}});
			return;
		}
		const json& ops = body.at("ops");
		auto errors = found->get().doc->applyFrame(seq, ops);
		framesApplied++;
		opsApplied += ops.is_array() ? ops.size() : 0;
		opErrors += errors.size();
		for (const ApplyError& error : errors) {
			lastError = "frame " + std::to_string(error.s) + " op " + std::to_string(error.op) + ": " + error.msg;
			sendEvent({{"ev", "error"}, {"sf", sf}, {"s", error.s}, {"op", error.op}, {"msg", error.msg}});
		}
		int64_t& acked = pendingAcks_[sf];
		acked = std::max(acked, seq);
		return;
	}
}

} // namespace tsp
