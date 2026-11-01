#include "pty.h"

#ifdef __APPLE__
#include <crt_externs.h>
#include <util.h>
#else
#include <pty_system.h>
#endif
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

namespace {

std::string nativeError(std::string_view operation, int error = errno) {
	return std::string(operation) + " failed: " + std::error_code(error, std::generic_category()).message();
}

class Fd {
public:
	Fd() = default;
	explicit Fd(int value) : value_(value) {}
	~Fd() { reset(); }
	Fd(const Fd&) = delete;
	Fd& operator=(const Fd&) = delete;
	Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
	Fd& operator=(Fd&& other) noexcept {
		if (this != &other) {
			reset();
			value_ = std::exchange(other.value_, -1);
		}
		return *this;
	}
	int get() const { return value_; }
	void reset() {
		// Retrying close after EINTR can close a reused descriptor on Linux.
		if (value_ >= 0)
			::close(value_);
		value_ = -1;
	}

private:
	int value_ = -1;
};

bool configureFd(int fd, bool nonblocking, std::string& err) {
	const int descriptorFlags = fcntl(fd, F_GETFD);
	if (descriptorFlags == -1 || fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) == -1) {
		err = nativeError("fcntl(FD_CLOEXEC)");
		return false;
	}
	if (nonblocking) {
		const int statusFlags = fcntl(fd, F_GETFL);
		if (statusFlags == -1 || fcntl(fd, F_SETFL, statusFlags | O_NONBLOCK) == -1) {
			err = nativeError("fcntl(O_NONBLOCK)");
			return false;
		}
	}
	return true;
}

bool makePipe(Fd& read, Fd& write, bool nonblocking, std::string& err) {
	std::array<int, 2> descriptors;
#ifdef __linux__
	const int result = pipe2(descriptors.data(), O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0));
#else
	const int result = pipe(descriptors.data());
#endif
	if (result == -1) {
		err = nativeError("pipe");
		return false;
	}
	read = Fd(descriptors[0]);
	write = Fd(descriptors[1]);
#ifdef __linux__
	return true;
#else
	return configureFd(read.get(), nonblocking, err) && configureFd(write.get(), nonblocking, err);
#endif
}

bool equalName(std::string_view left, std::string_view right) {
	if (left.size() != right.size())
		return false;
	for (size_t index = 0; index < left.size(); ++index) {
		const auto upper = [](char ch) {
			return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch;
		};
		if (upper(left[index]) != upper(right[index]))
			return false;
	}
	return true;
}

std::vector<std::string> childEnvironment() {
	constexpr std::array<std::string_view, 9> dropped = {
	    "WT_SESSION", "WT_PROFILE_ID", "TERM_PROGRAM", "TERM_PROGRAM_VERSION", "TERM", "COLORTERM",
	    "TMUX",       "STY",           "ZELLIJ"};
	std::vector<std::string> result;
#ifdef __APPLE__
	const auto inherited = *_NSGetEnviron();
#else
	const auto inherited = environ;
#endif
	for (size_t index = 0; inherited[index]; ++index) {
		const std::string_view entry(inherited[index]);
		const auto name = entry.substr(0, entry.find('='));
		bool drop = false;
		for (const auto candidate : dropped) {
			if (equalName(name, candidate)) {
				drop = true;
				break;
			}
		}
		if (!drop)
			result.emplace_back(entry);
	}
	result.emplace_back("TERM=xterm-256color");
	result.emplace_back("COLORTERM=truecolor");
	result.emplace_back("TERM_PROGRAM=omp-ui");
	return result;
}

enum class ChildStage : int { Directory, Exec };
struct ChildError {
	ChildStage stage;
	int error;
};

[[noreturn]] void childError(int fd, ChildStage stage) {
	const ChildError error{stage, errno};
	auto bytes = std::string_view(reinterpret_cast<const char*>(&error), sizeof(error));
	while (!bytes.empty()) {
		const ssize_t count = ::write(fd, bytes.data(), bytes.size());
		if (count > 0)
			bytes.remove_prefix(static_cast<size_t>(count));
		else if (count == -1 && errno == EINTR)
			continue;
		else
			break;
	}
	_exit(127);
}

int exitCode(int status) {
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 1;
}

int pollUntilReady(std::span<pollfd> descriptors) {
	int result;
	do {
		result = poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), -1);
	} while (result == -1 && errno == EINTR);
	return result;
}

} // namespace

struct Pty::State {
	Fd master, wakeRead, wakeWrite;
	pid_t child = -1;
	std::mutex writeMutex, processMutex, closeMutex;
	std::atomic<bool> writable{true}, exited{false};
	int code = 1;
	bool closed = false;
	std::jthread reader, waiter;

	~State() { shutdown(); }

	void reap() {
		// Observe without reaping first: close() must never signal a reused pid.
		siginfo_t information{};
		int observed;
		do {
			observed = waitid(P_PID, static_cast<id_t>(child), &information, WEXITED | WNOWAIT);
		} while (observed == -1 && errno == EINTR);
		{
			std::lock_guard processLock(processMutex);
			int status = 0;
			pid_t result;
			do {
				result = waitpid(child, &status, 0);
			} while (result == -1 && errno == EINTR);
			code = result == child ? exitCode(status) : 1;
			writable.store(false);
			exited.store(true, std::memory_order_release);
		}
		const char wake = 0;
		ssize_t result;
		do {
			result = ::write(wakeWrite.get(), &wake, sizeof(wake));
		} while (result == -1 && errno == EINTR);
	}

	void drain(const DataFn& onData) {
		std::array<char, 16384> buffer;
		for (;;) {
			const ssize_t count = ::read(master.get(), buffer.data(), buffer.size());
			if (count > 0) {
				if (onData)
					onData(std::string_view(buffer.data(), static_cast<size_t>(count)));
			} else if (count == -1 && errno == EINTR) {
				continue;
			} else {
				break; // EOF/EIO, or no queued output from a surviving descendant.
			}
		}
	}

	void pump(std::stop_token stop, const DataFn& onData, const ExitFn& onExit) {
		std::array<char, 16384> buffer;
		bool streamEnded = false;
		while (!stop.stop_requested()) {
			if (exited.load(std::memory_order_acquire)) {
				drain(onData);
				break;
			}
			if (!streamEnded) {
				const ssize_t count = ::read(master.get(), buffer.data(), buffer.size());
				if (count > 0) {
					if (onData)
						onData(std::string_view(buffer.data(), static_cast<size_t>(count)));
					continue;
				}
				if (count == -1 && errno == EINTR)
					continue;
				if (count == 0 || (count == -1 && errno != EAGAIN && errno != EWOULDBLOCK))
					streamEnded = true; // Linux reports EIO when the slave closes.
			}
			std::array<pollfd, 2> ready{{{streamEnded ? -1 : master.get(), POLLIN, 0}, {wakeRead.get(), POLLIN, 0}}};
			if (pollUntilReady(ready) == -1)
				streamEnded = true;
		}
		if (exited.load(std::memory_order_acquire) && onExit)
			onExit(code);
	}

	void shutdown() {
		std::lock_guard closeLock(closeMutex);
		if (closed)
			return;
		closed = true;
		writable.store(false);
		{
			std::lock_guard processLock(processMutex);
			if (child > 0 && !exited.load(std::memory_order_acquire)) {
				const pid_t foreground = tcgetpgrp(master.get());
				if (foreground > 0 && foreground != child)
					kill(-foreground, SIGKILL);
				if (kill(-child, SIGKILL) == -1)
					kill(child, SIGKILL);
			}
		}
		// Startup failure can occur before either thread exists.
		if (child > 0 && !waiter.joinable())
			reap();
		if (reader.joinable())
			reader.join();
		if (waiter.joinable())
			waiter.join();
		{
			std::lock_guard writeLock(writeMutex);
			master.reset();
		}
		wakeRead.reset();
		wakeWrite.reset();
	}
};

Pty::Pty() = default;
Pty::~Pty() = default;
Pty::Pty(Pty&&) noexcept = default;
Pty& Pty::operator=(Pty&&) noexcept = default;

bool Pty::start(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows, DataFn onData,
                ExitFn onExit, std::string& err) {
	close();
	err.clear();
	if (argv.empty()) {
		err = "cannot start an empty command";
		return false;
	}
	auto next = std::make_unique<State>();
	Fd errorRead, errorWrite;
	if (!makePipe(errorRead, errorWrite, false, err) || !makePipe(next->wakeRead, next->wakeWrite, true, err))
		return false;
	auto environment = childEnvironment();
	// These mutable pointer arrays exist only for the execvp/environ C boundary;
	// the pointed-to strings stay owned by argv and environment until exec.
	std::vector<char*> arguments;
	arguments.reserve(argv.size() + 1);
	for (const auto& argument : argv)
		arguments.push_back(const_cast<char*>(argument.c_str()));
	arguments.push_back(nullptr);
	std::vector<char*> environmentPointers;
	environmentPointers.reserve(environment.size() + 1);
	for (auto& entry : environment)
		environmentPointers.push_back(entry.data());
	environmentPointers.push_back(nullptr);
	winsize size{};
	size.ws_col = static_cast<unsigned short>(cols);
	size.ws_row = static_cast<unsigned short>(rows);
	int master = -1;
	const pid_t child = forkpty(&master, nullptr, nullptr, &size);
	if (child == 0) {
		errorRead.reset();
		if (!cwd.empty() && chdir(cwd.c_str()) == -1)
			childError(errorWrite.get(), ChildStage::Directory);
		// No allocation or environment mutation routines in the forked child.
#ifdef __APPLE__
		*_NSGetEnviron() = environmentPointers.data();
#else
		environ = environmentPointers.data();
#endif
		execvp(arguments.front(), arguments.data());
		childError(errorWrite.get(), ChildStage::Exec);
	}
	if (child == -1) {
		err = nativeError("forkpty");
		return false;
	}
	next->child = child;
	next->master = Fd(master);
	errorWrite.reset();
	ChildError failure{};
	size_t received = 0;
	while (received < sizeof(failure)) {
		const ssize_t count =
		    ::read(errorRead.get(), reinterpret_cast<char*>(&failure) + received, sizeof(failure) - received);
		if (count > 0)
			received += static_cast<size_t>(count);
		else if (count == 0)
			break; // Successful exec closes the error pipe.
		else if (errno != EINTR) {
			err = nativeError("read(exec error)");
			return false;
		}
	}
	if (received) {
		err = received == sizeof(failure)
		        ? nativeError(failure.stage == ChildStage::Directory ? "chdir" : "execvp", failure.error)
		        : "incomplete child startup error";
		return false;
	}
	if (!configureFd(next->master.get(), true, err))
		return false;
	// Publish before callbacks can send the initial protocol replies.
	state_ = std::move(next);
	auto& state = *state_;
	state.reader = std::jthread([&state, onData = std::move(onData), onExit = std::move(onExit)](std::stop_token stop) {
		state.pump(stop, onData, onExit);
	});
	state.waiter = std::jthread([&state] { state.reap(); });
	return true;
}

void Pty::write(std::string_view data) {
	if (!state_ || !state_->writable.load())
		return;
	std::lock_guard lock(state_->writeMutex);
	while (state_->writable.load() && !data.empty()) {
		const ssize_t count = ::write(state_->master.get(), data.data(), std::min(data.size(), size_t{16384}));
		if (count > 0) {
			data.remove_prefix(static_cast<size_t>(count));
		} else if (count == -1 && errno == EINTR) {
			continue;
		} else if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			std::array<pollfd, 2> ready{{{state_->master.get(), POLLOUT, 0}, {state_->wakeRead.get(), POLLIN, 0}}};
			if (pollUntilReady(ready) == -1 || (ready[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
				return;
		} else {
			return;
		}
	}
}

void Pty::resize(int cols, int rows) {
	if (!state_)
		return;
	std::lock_guard lock(state_->writeMutex);
	if (!state_->writable.load())
		return;
	winsize size{};
	size.ws_col = static_cast<unsigned short>(cols);
	size.ws_row = static_cast<unsigned short>(rows);
	ioctl(state_->master.get(), TIOCSWINSZ, &size);
}

void Pty::close() {
	if (state_)
		state_->shutdown();
}

std::string_view Pty::backend() const {
	return "posix";
}

uint64_t Pty::pid() const {
	return state_ && !state_->closed ? static_cast<uint64_t>(state_->child) : 0;
}
