#include "pty.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <mutex>
#include <thread>
#include <utility>

namespace {

using CreateConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ResizeConsoleFn = HRESULT(WINAPI*)(HPCON, COORD);
using CloseConsoleFn = VOID(WINAPI*)(HPCON);

std::string lastError(std::string_view operation) {
	return std::string(operation) + " failed (error " + std::to_string(GetLastError()) + ")";
}

class Handle {
public:
	Handle() = default;
	explicit Handle(HANDLE value) : value_(value) {}
	~Handle() { reset(); }
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
	Handle& operator=(Handle&& other) noexcept {
		if (this != &other) {
			reset();
			value_ = std::exchange(other.value_, nullptr);
		}
		return *this;
	}
	HANDLE get() const { return value_; }
	explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
	void reset() {
		if (*this)
			CloseHandle(value_);
		value_ = nullptr;
	}

private:
	HANDLE value_ = nullptr;
};

class Library {
public:
	Library() = default;
	explicit Library(HMODULE value) : value_(value) {}
	~Library() { reset(); }
	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;
	Library(Library&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
	Library& operator=(Library&& other) noexcept {
		if (this != &other) {
			reset();
			value_ = std::exchange(other.value_, nullptr);
		}
		return *this;
	}
	HMODULE get() const { return value_; }
	void reset() {
		if (value_)
			FreeLibrary(value_);
		value_ = nullptr;
	}

private:
	HMODULE value_ = nullptr;
};

class Console {
public:
	Console() = default;
	Console(HPCON value, CloseConsoleFn close) : value_(value), close_(close) {}
	~Console() { reset(); }
	Console(const Console&) = delete;
	Console& operator=(const Console&) = delete;
	Console(Console&& other) noexcept : value_(std::exchange(other.value_, nullptr)), close_(other.close_) {}
	Console& operator=(Console&& other) noexcept {
		if (this != &other) {
			reset();
			value_ = std::exchange(other.value_, nullptr);
			close_ = other.close_;
		}
		return *this;
	}
	HPCON get() const { return value_; }
	void reset() {
		if (value_)
			close_(std::exchange(value_, nullptr));
	}

private:
	HPCON value_ = nullptr;
	CloseConsoleFn close_ = nullptr;
};

struct ConsoleApi {
	Library library;
	CreateConsoleFn create = nullptr;
	ResizeConsoleFn resize = nullptr;
	CloseConsoleFn close = nullptr;
	std::string_view backend = "kernel32";

	bool load(std::string& err) {
		std::wstring path(MAX_PATH, L'\0');
		for (;;) {
			const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
			if (!length) {
				err = lastError("GetModuleFileNameW");
				return false;
			}
			if (length < path.size()) {
				path.resize(length);
				break;
			}
			path.resize(path.size() * 2);
		}
		path.resize(path.find_last_of(L"\\/") + 1);
		path += L"conpty.dll";
		library = Library(LoadLibraryW(path.c_str()));
		if (library.get()) {
			create = reinterpret_cast<CreateConsoleFn>(GetProcAddress(library.get(), "ConptyCreatePseudoConsole"));
			resize = reinterpret_cast<ResizeConsoleFn>(GetProcAddress(library.get(), "ConptyResizePseudoConsole"));
			close = reinterpret_cast<CloseConsoleFn>(GetProcAddress(library.get(), "ConptyClosePseudoConsole"));
			if (create && resize && close) {
				backend = "conpty.dll";
				return true;
			}
			library.reset();
		}
		const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		create = reinterpret_cast<CreateConsoleFn>(GetProcAddress(kernel, "CreatePseudoConsole"));
		resize = reinterpret_cast<ResizeConsoleFn>(GetProcAddress(kernel, "ResizePseudoConsole"));
		close = reinterpret_cast<CloseConsoleFn>(GetProcAddress(kernel, "ClosePseudoConsole"));
		if (create && resize && close)
			return true;
		err = "no pseudo-console API available";
		return false;
	}
};

bool wideString(std::string_view text, std::wstring& result, std::string& err) {
	if (text.empty()) {
		result.clear();
		return true;
	}
	const int count =
	    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (!count) {
		err = lastError("MultiByteToWideChar");
		return false;
	}
	result.resize(count);
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
	                         count)) {
		err = lastError("MultiByteToWideChar");
		return false;
	}
	return true;
}

void appendArgument(std::wstring& command, std::wstring_view argument) {
	if (!command.empty())
		command += L' ';
	command += L'"';
	size_t slashes = 0;
	for (const wchar_t ch : argument) {
		if (ch == L'\\') {
			++slashes;
			continue;
		}
		command.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		command += ch;
		slashes = 0;
	}
	// Backslashes preceding the closing quote must survive CRT argv parsing.
	command.append(slashes * 2, L'\\');
	command += L'"';
}

struct EnvironmentDeleter {
	void operator()(wchar_t* block) const { FreeEnvironmentStringsW(block); }
};

bool childEnvironment(std::vector<wchar_t>& block, std::string& err) {
	constexpr std::array<std::wstring_view, 9> dropped = {
	    L"WT_SESSION", L"WT_PROFILE_ID", L"TERM_PROGRAM", L"TERM_PROGRAM_VERSION", L"TERM", L"COLORTERM",
	    L"TMUX",       L"STY",           L"ZELLIJ"};
	const std::unique_ptr<wchar_t, EnvironmentDeleter> environment(GetEnvironmentStringsW());
	if (!environment) {
		err = lastError("GetEnvironmentStringsW");
		return false;
	}
	for (size_t offset = 0; environment.get()[offset];) {
		const std::wstring_view entry(environment.get() + offset);
		bool drop = false;
		for (const auto name : dropped) {
			if (entry.size() > name.size() && entry[name.size()] == L'='
			    && _wcsnicmp(entry.data(), name.data(), name.size()) == 0) {
				drop = true;
				break;
			}
		}
		if (!drop) {
			block.insert(block.end(), entry.begin(), entry.end());
			block.push_back(L'\0');
		}
		offset += entry.size() + 1;
	}
	for (const std::wstring_view entry : {L"TERM=xterm-256color", L"COLORTERM=truecolor", L"TERM_PROGRAM=omp-ui"}) {
		block.insert(block.end(), entry.begin(), entry.end());
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	return true;
}

bool makePipe(Handle& read, Handle& write, std::string& err) {
	HANDLE readValue = nullptr;
	HANDLE writeValue = nullptr;
	const BOOL ok = CreatePipe(&readValue, &writeValue, nullptr, 0);
	read = Handle(readValue);
	write = Handle(writeValue);
	if (!ok)
		err = lastError("CreatePipe");
	return ok != FALSE;
}

class ProcessAttributes {
public:
	~ProcessAttributes() {
		if (initialized_)
			DeleteProcThreadAttributeList(get());
	}
	ProcessAttributes() = default;
	ProcessAttributes(const ProcessAttributes&) = delete;
	ProcessAttributes& operator=(const ProcessAttributes&) = delete;
	LPPROC_THREAD_ATTRIBUTE_LIST get() { return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data()); }
	bool initialize(HPCON console, std::string& err) {
		SIZE_T bytes = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
		storage_.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
		if (!InitializeProcThreadAttributeList(get(), 1, 0, &bytes)) {
			err = lastError("InitializeProcThreadAttributeList");
			return false;
		}
		initialized_ = true;
		if (!UpdateProcThreadAttribute(get(), 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(console), nullptr,
		                               nullptr)) {
			err = lastError("UpdateProcThreadAttribute");
			return false;
		}
		return true;
	}

private:
	std::vector<std::max_align_t> storage_;
	bool initialized_ = false;
};

} // namespace

struct Pty::State {
	ConsoleApi api;
	Console console;
	Handle input, output, process;
	DWORD child = 0;
	std::mutex writeMutex, consoleMutex, closeMutex;
	std::atomic<bool> writable{true};
	bool closed = false;
	std::jthread reader, waiter;

	~State() { shutdown(); }

	void closeConsole() {
		std::lock_guard lock(consoleMutex);
		console.reset();
	}

	void shutdown() {
		std::lock_guard closeLock(closeMutex);
		if (closed)
			return;
		closed = true;
		writable.store(false);
		if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT)
			TerminateProcess(process.get(), 1);
		// This blocks until output drains. Never stop/join the reader first.
		closeConsole();
		{
			std::lock_guard writeLock(writeMutex);
			input.reset();
		}
		if (reader.joinable())
			reader.join();
		if (waiter.joinable())
			waiter.join();
		output.reset();
		process.reset();
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
	std::wstring command, argument, directory;
	for (const auto& value : argv) {
		if (!wideString(value, argument, err))
			return false;
		appendArgument(command, argument);
	}
	if (!wideString(cwd, directory, err))
		return false;
	std::vector<wchar_t> environment;
	if (!childEnvironment(environment, err))
		return false;
	auto next = std::make_unique<State>();
	if (!next->api.load(err))
		return false;
	Handle inputRead, outputWrite;
	if (!makePipe(inputRead, next->input, err) || !makePipe(next->output, outputWrite, err))
		return false;
	HPCON console = nullptr;
	const HRESULT result = next->api.create(COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)}, inputRead.get(),
	                                        outputWrite.get(), 0, &console);
	next->console = Console(console, next->api.close);
	// ConPTY duplicated these ends; keeping ours would prevent output EOF.
	inputRead.reset();
	outputWrite.reset();
	if (FAILED(result)) {
		err = "CreatePseudoConsole failed (hr " + std::to_string(result) + ")";
		return false;
	}
	ProcessAttributes attributes;
	if (!attributes.initialize(next->console.get(), err))
		return false;
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	// Redirected parent handles must not replace the child's console handles.
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.lpAttributeList = attributes.get();
	PROCESS_INFORMATION information{};
	const BOOL created = CreateProcessW(
	    nullptr, command.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
	    environment.data(), directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &information);
	next->process = Handle(information.hProcess);
	const Handle processThread(information.hThread);
	if (!created) {
		err = lastError("CreateProcessW");
		return false;
	}
	next->child = information.dwProcessId;
	// Publish before callbacks can send the initial protocol replies.
	state_ = std::move(next);
	auto& state = *state_;
	state.reader = std::jthread([&state, onData = std::move(onData), onExit = std::move(onExit)](std::stop_token stop) {
		std::array<char, 16384> buffer;
		DWORD count = 0;
		while (!stop.stop_requested()
		       && ReadFile(state.output.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)
		       && count) {
			if (onData)
				onData(std::string_view(buffer.data(), count));
		}
		WaitForSingleObject(state.process.get(), INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(state.process.get(), &code);
		if (onExit)
			onExit(static_cast<int>(code));
	});
	state.waiter = std::jthread([&state] {
		WaitForSingleObject(state.process.get(), INFINITE);
		state.writable.store(false);
		// Natural exit also needs to close ConPTY so the reader can observe EOF.
		state.closeConsole();
	});
	return true;
}

void Pty::write(std::string_view data) {
	if (!state_ || !state_->writable.load())
		return;
	std::lock_guard lock(state_->writeMutex);
	while (state_->writable.load() && !data.empty()) {
		DWORD count = 0;
		const DWORD size = static_cast<DWORD>(std::min(data.size(), size_t{16384}));
		if (!WriteFile(state_->input.get(), data.data(), size, &count, nullptr) || !count)
			return;
		data.remove_prefix(count);
	}
}

void Pty::resize(int cols, int rows) {
	if (!state_)
		return;
	std::lock_guard lock(state_->consoleMutex);
	if (state_->writable.load())
		state_->api.resize(state_->console.get(), COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
}

void Pty::close() {
	if (state_)
		state_->shutdown();
}

std::string_view Pty::backend() const {
	return state_ ? state_->api.backend : "kernel32";
}

uint64_t Pty::pid() const {
	return state_ && !state_->closed ? static_cast<uint64_t>(state_->child) : 0;
}
