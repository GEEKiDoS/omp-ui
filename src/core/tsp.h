// Tern Surface Protocol terminal: framing, handle-based documents and sessions.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace tsp {

constexpr int kVersion = 1;
constexpr size_t kApcLimit = 65536;

double nowMs();

struct RawMessage {
	std::string verb;
	std::map<std::string, std::string> params;
	std::string body;
};

std::optional<RawMessage> splitMessage(std::string_view apcBody);
/** Terminal → program: OSC 877 on Windows (ConPTY drops APC), APC elsewhere. */
std::string encodeReply(char verb, const std::string& body, size_t limit = kApcLimit);

enum class Kind : uint8_t {
	Col,
	Row,
	Card,
	Section,
	Rule,
	Spacer,
	Text,
	Md,
	Code,
	Diff,
	Ansi,
	Math,
	Image,
	Media,
	Kv,
	Table,
	Tree,
	Json,
	Badge,
	Kbd,
	Icon,
	Spinner,
	Shimmer,
	Elapsed,
	Progress,
	Rate,
	List,
	Item,
	Tabs,
	Editor,
	Input,
	Status,
	Seg,
	Overlay,
	Toast,
	Rows,
	Picker,
	Prefs,
	Tool,
	Checklist,
	Agent,
	Chart,
	Meter,
	Effort
};
std::optional<Kind> parseKind(std::string_view name);
std::string_view kindName(Kind kind);
bool isTextKind(Kind kind);

using NodeId = uint32_t; // Document-local, recycled slot; zero is not a node.

struct Node {
	std::string id;
	Kind kind = Kind::Col;
	json p = json::object();
	std::vector<NodeId> children;
	NodeId parent = 0;
	uint64_t rev = 0;
	std::unordered_map<std::string, double> stamps;

	const json& prop(std::string_view key) const;
	std::string_view str(std::string_view key, std::string_view fallback = {}) const;
	double num(std::string_view key, double fallback = 0) const;
	bool flag(std::string_view key, bool fallback = false) const;
	double since(std::string_view key) const;
};

struct ApplyError {
	int64_t s;
	int op;
	std::string msg;
};

struct DocHooks {
	std::function<void(NodeId)> added;
	std::function<void(NodeId)> removing;
	std::function<void(NodeId)> moved;
	std::function<void(NodeId, const json& changed)> propsChanged;
	std::function<void(NodeId, size_t fromByte)> textChanged;
	std::function<void(std::optional<NodeId>)> focusChanged;
};

class Document {
public:
	explicit Document(std::string surface);
	~Document();
	Document(const Document&) = delete;
	Document& operator=(const Document&) = delete;

	NodeId root() const { return root_; }
	[[nodiscard]] std::optional<NodeId> find(std::string_view id) const;
	Node& at(NodeId id) { return nodes_[id]; }
	const Node& at(NodeId id) const { return nodes_[id]; }
	std::optional<NodeId> focus() const { return focus_; }
	bool suspended() const { return suspended_; }
	size_t size() const { return index_.size(); }
	uint64_t version() const { return version_; }
	std::vector<std::pair<std::string, std::string>> reveals, scrolls;

	void attach(DocHooks hooks) { hooks_ = std::move(hooks); }
	void detach() { hooks_ = {}; }
	/** Drop live-only dock/layer regions; retain main and its view. */
	void close();
	/** Notify and remove root children while their views are still alive. */
	void teardown();
	std::vector<ApplyError> applyFrame(int64_t s, const json& ops);
	json snapshot() const;

private:
	void apply(const json& op);
	NodeId node(const std::string& id) const;
	NodeId textNode(const std::string& id) const;
	size_t insertIndex(NodeId parent, const json& before) const;
	NodeId allocate();
	NodeId build(const json& wire, NodeId parent, std::unordered_set<std::string>& seen, std::vector<NodeId>& built);
	void mergeProps(Node& n, const json& props, bool fromSet);
	void changeFocus(std::optional<NodeId> focus);
	void removeSubtree(NodeId id);
	void unregister(NodeId id);
	json exportNode(NodeId id) const;

	std::deque<Node> nodes_;
	std::vector<NodeId> free_;
	std::unordered_map<std::string, NodeId> index_;
	NodeId root_ = 0;
	std::optional<NodeId> focus_;
	DocHooks hooks_;
	bool suspended_ = false;
	uint64_t version_ = 0;
};

struct Surface {
	std::string id;
	std::string mode;
	std::string title, role;
	bool open = true;
	std::unique_ptr<Document> doc;
};

struct LogEntry {
	bool out;
	std::string verb;
	size_t bytes;
	std::string text;
};

class Session {
public:
	using WriteFn = std::function<void(const std::string&)>;
	explicit Session(WriteFn write) : write_(std::move(write)) {}
	~Session();

	std::vector<std::string> kinds, features;
	int credits = 4;
	int cols = 100;
	float cellW = 8, cellH = 16;
	bool dark = true;
	std::string term = "omp-ui", termVersion = "0.1";

	bool onApc(std::string_view body);
	void flushAcks();
	void sendEvent(const json& event);
	void resize(int newCols, float newCellW, float newCellH);

	bool helloAnswered() const { return helloAnswered_; }
	const std::string& programName() const { return app_; }
	const std::string& programVersion() const { return appVersion_; }
	const std::vector<std::string>& programFeatures() const { return appFeatures_; }
	bool programFeature(const std::string& name) const;

	std::vector<std::unique_ptr<Surface>>& surfaces() { return surfaces_; }
	std::optional<std::reference_wrapper<Surface>> find(std::string_view id);
	std::optional<std::reference_wrapper<Surface>> screen();
	std::function<void(Surface&)> surfaceOpened;
	std::function<void(Surface&)> surfaceClosing;
	std::function<void(Surface&)> surfaceChanged;
	void teardownAll();
	// Image bytes (base64, as received with verb `b`) by content address; frames may reference a
	// blob before it arrives, so `blobAdded` tells the view to look again.
	std::optional<std::string_view> blob(std::string_view id) const;
	std::function<void(const std::string& id)> blobAdded;

	std::deque<LogEntry> log;
	size_t logLimit = 400;
	std::function<void(const LogEntry&)> onLog;
	uint64_t framesApplied = 0, opsApplied = 0, opErrors = 0;
	std::string lastError;
	uint64_t version = 0;

private:
	void dispatch(const RawMessage& msg);
	void reply(const json& body);
	void record(bool out, const std::string& verb, const std::string& body);
	void closing(Surface& surface);

	WriteFn write_;
	bool helloAnswered_ = false;
	std::string app_, appVersion_;
	std::vector<std::string> appFeatures_;
	std::vector<std::unique_ptr<Surface>> surfaces_;
	// A chunked message keeps its first chunk's parameters (`id` for blobs) while bodies accumulate.
	struct Chunked {
		std::map<std::string, std::string> params;
		std::string body;
	};
	std::unordered_map<std::string, Chunked> chunks_;
	std::unordered_map<std::string, std::string> blobs_;
	std::map<std::string, int64_t> pendingAcks_;
};

} // namespace tsp
