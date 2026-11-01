// Splits a pty output byte stream into plain text and control sequences.
// Escape sequences may straddle reads; the parser keeps partial state.
// Only the shapes a TSP host needs are surfaced: CSI (to answer device
// queries and track modes), OSC (title, colour queries) and APC (TSP).
#pragma once

#include <string>
#include <string_view>

class VtHandler {
public:
	virtual ~VtHandler() = default;
	/** Ground-state bytes, control characters included (UTF-8, possibly split mid-character). */
	virtual void onText(std::string_view text) = 0;
	/** `CSI <prefix><params><intermediates><final>`; `prefix` is one of `<=>?` or 0. */
	virtual void onCsi(char prefix, std::string_view params, std::string_view intermediates, char final) = 0;
	/** OSC body without introducer and terminator. */
	virtual void onOsc(std::string_view body) = 0;
	/** APC body without introducer and terminator. */
	virtual void onApc(std::string_view body) = 0;
};

class VtParser {
public:
	explicit VtParser(VtHandler& handler) : handler_(handler) {}
	void feed(std::string_view bytes);

private:
	enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, OscEscape, Apc, ApcEscape, Ignore, IgnoreEscape };

	void flushText();

	VtHandler& handler_;
	State state_ = State::Ground;
	std::string text_;
	std::string seq_;
};
