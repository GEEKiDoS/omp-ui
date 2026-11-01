#include "vt.h"

namespace {
/** Larger strings are dropped rather than buffered without bound. */
constexpr size_t kMaxSequence = 16 * 1024 * 1024;
} // namespace

void VtParser::flushText() {
	if (text_.empty())
		return;
	handler_.onText(text_);
	text_.clear();
}

void VtParser::feed(std::string_view bytes) {
	for (size_t i = 0; i < bytes.size(); i++) {
		const unsigned char b = static_cast<unsigned char>(bytes[i]);
		switch (state_) {
		case State::Ground:
			if (b == 0x1b) {
				state_ = State::Escape;
			} else {
				text_.push_back(static_cast<char>(b));
			}
			break;

		case State::Escape:
			seq_.clear();
			switch (b) {
			case '[':
				state_ = State::Csi;
				break;
			case ']':
				state_ = State::Osc;
				break;
			case '_':
				state_ = State::Apc;
				break;
			case 'P':
			case '^':
			case 'X':
				state_ = State::Ignore;
				break;
			case 0x1b:
				break; // ESC ESC: the second one starts over.
			default:
				if (b >= 0x20 && b <= 0x2f) {
					state_ = State::EscapeIntermediate; // e.g. `ESC ( B` charset designation.
				} else {
					state_ = State::Ground; // Two-byte escape (`ESC 7`, `ESC M`, …): no visible text.
				}
			}
			if (state_ != State::Ground && state_ != State::Escape)
				flushText();
			break;

		case State::EscapeIntermediate:
			if (b >= 0x30 && b <= 0x7e)
				state_ = State::Ground;
			else if (b == 0x1b)
				state_ = State::Escape;
			break;

		case State::Csi:
			if (b == 0x1b) {
				state_ = State::Escape;
			} else if (b >= 0x40 && b <= 0x7e) {
				std::string_view s = seq_;
				char prefix = 0;
				if (!s.empty() && (s[0] == '<' || s[0] == '=' || s[0] == '>' || s[0] == '?')) {
					prefix = s[0];
					s.remove_prefix(1);
				}
				size_t split = s.size();
				while (split > 0 && static_cast<unsigned char>(s[split - 1]) >= 0x20
				       && static_cast<unsigned char>(s[split - 1]) <= 0x2f)
					split--;
				flushText();
				handler_.onCsi(prefix, s.substr(0, split), s.substr(split), static_cast<char>(b));
				state_ = State::Ground;
			} else if (b >= 0x20) {
				seq_.push_back(static_cast<char>(b));
			} else {
				text_.push_back(static_cast<char>(b)); // C0 control inside CSI executes immediately.
			}
			break;

		case State::Osc:
			if (b == 0x07) {
				handler_.onOsc(seq_);
				state_ = State::Ground;
			} else if (b == 0x1b) {
				state_ = State::OscEscape;
			} else if (seq_.size() < kMaxSequence) {
				seq_.push_back(static_cast<char>(b));
			}
			break;

		case State::OscEscape:
			if (b == '\\') {
				handler_.onOsc(seq_);
				state_ = State::Ground;
			} else {
				state_ = State::Escape; // Aborted string: this ESC starts a new sequence.
				i--;
			}
			break;

		case State::Apc:
			if (b == 0x1b) {
				state_ = State::ApcEscape;
			} else if (seq_.size() < kMaxSequence) {
				seq_.push_back(static_cast<char>(b));
			}
			break;

		case State::ApcEscape:
			if (b == '\\') {
				handler_.onApc(seq_);
				state_ = State::Ground;
			} else {
				state_ = State::Escape;
				i--;
			}
			break;

		case State::Ignore:
			if (b == 0x1b)
				state_ = State::IgnoreEscape;
			break;

		case State::IgnoreEscape:
			if (b == '\\') {
				state_ = State::Ground;
			} else {
				state_ = State::Escape;
				i--;
			}
			break;
		}
	}
	if (state_ == State::Ground)
		flushText();
}
