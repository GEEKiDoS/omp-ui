// The terminal side of the pty: answers the device queries omp's probes end
// with (DA1 is the sentinel behind every capability probe, including TSP's
// `hello`), tracks the input modes it requests, and hands APC strings to the
// TSP session. Non-TSP output is kept as plain text for the fallback view.
#pragma once

#include "tsp.h"
#include "vt.h"

#include <array>
#include <functional>
#include <string>

class TerminalHost : public VtHandler {
public:
	using WriteFn = std::function<void(const std::string&)>;

	explicit TerminalHost(WriteFn write);

	void feed(std::string_view bytes) { parser_.feed(bytes); }

	tsp::Session session;
	/** Program asked for win32-input-mode (`CSI ? 9001 h`): keys go as key records. */
	bool win32InputMode = false;
	/** Program enabled bracketed paste (`CSI ? 2004 h`). */
	bool bracketedPaste = false;
	/** Supported kitty enhancement: flag 1 (disambiguate escape codes). */
	unsigned kittyFlags = 0;
	/** xterm modifyOtherKeys level (0 disables, 1/2 enable modified key reports). */
	unsigned modifyOtherKeys = 0;
	std::string title;
	/** A desktop notification (OSC 9/99/777) or a bell (BEL, empty message) arrived while feed() ran. */
	std::function<void(std::string_view message)> onNotification;
	/** Visible text of non-TSP output (escapes stripped), capped. */
	std::string rowsText;
	size_t rowsLimit = 256 * 1024;
	int rows = 40;
	/** Background answered to OSC 11 queries, `rgb:rrrr/gggg/bbbb`. */
	std::string background = "rgb:1717/1919/1f1f";
	std::string foreground = "rgb:d8d8/dcdc/e4e4";

	void onText(std::string_view text) override;
	void onCsi(char prefix, std::string_view params, std::string_view intermediates, char final) override;
	void onOsc(std::string_view body) override;
	void onApc(std::string_view body) override;

private:
	WriteFn write_;
	VtParser parser_;
	struct KittyState {
		unsigned flags = 0;
		std::array<unsigned, 16> stack{};
		size_t depth = 0;
	};
	std::array<KittyState, 2> kitty_{};
	bool alternateScreen_ = false;
};
