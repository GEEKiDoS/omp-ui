#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct EnvironmentOverride {
	std::string name;
	std::optional<std::string> value;
};
using EnvironmentOverrides = std::vector<EnvironmentOverride>;

// One child and its terminal. Callbacks run off the owning/UI thread; data views
// are valid only for the duration of the callback. close() drains before return.
class Pty {
public:
	using DataFn = std::function<void(std::string_view)>;
	using ExitFn = std::function<void(int code)>;

	Pty();
	~Pty();
	Pty(const Pty&) = delete;
	Pty& operator=(const Pty&) = delete;
	Pty(Pty&&) noexcept;
	Pty& operator=(Pty&&) noexcept;

	bool start(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows,
	           const EnvironmentOverrides& environment, DataFn onData, ExitFn onExit, std::string& err);

	void write(std::string_view data);
	void resize(int cols, int rows);
	void close();
	[[nodiscard]] std::string_view backend() const;
	/** Child process ID; zero before start() or after close(). */
	[[nodiscard]] uint64_t pid() const;

private:
	struct State;
	std::unique_ptr<State> state_;
};
