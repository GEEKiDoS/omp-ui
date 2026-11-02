#pragma once

#include <cstdint>

namespace gfx {

// Applied with the theme; disabling transitions also settles ones already in flight.
inline bool animations = true;

// A small, allocation-free cubic ease-out, sampled with SDL_GetTicks().
class Tween {
public:
	float value(std::uint64_t now) const {
		if (!active(now))
			return target_;
		const float left = 1 - static_cast<float>(now - started_) / static_cast<float>(duration_);
		return target_ + (from_ - target_) * left * left * left;
	}
	float target() const { return target_; }
	bool active(std::uint64_t now) const { return animations && duration_ && now - started_ < duration_; }
	void snap(float value) {
		from_ = target_ = value;
		duration_ = 0;
	}
	void to(float value, std::uint64_t now, std::uint64_t duration = 160) {
		if (value == target_)
			return;
		from_ = this->value(now);
		target_ = value;
		started_ = now;
		duration_ = animations ? duration : 0;
	}

private:
	float from_ = 0, target_ = 0;
	std::uint64_t started_ = 0, duration_ = 0;
};

} // namespace gfx
