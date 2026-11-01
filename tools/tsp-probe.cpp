// tsp-probe: headless Tern Surface Protocol terminal. Runs a program (omp by
// default) under a pty, answers its `hello`, applies and acknowledges every
// frame, and prints the protocol traffic. Exit status 0 means a surface was
// opened and frames were applied without rejected ops.
//
//   tsp-probe [--seconds N] [--type TEXT] [--dump FILE] [-- command line]
#include "host.h"
#include "pty.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	constexpr std::string_view replyFraming = "OSC 877";
#else
	constexpr std::string_view replyFraming = "APC";
#endif
	double seconds = 15;
	std::string typed, dumpPath;
	std::vector<std::string> command{"omp"};
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--seconds" && i + 1 < argc) {
			seconds = std::atof(argv[++i]);
		} else if (a == "--type" && i + 1 < argc) {
			typed = argv[++i];
		} else if (a == "--dump" && i + 1 < argc) {
			dumpPath = argv[++i];
		} else if (a == "--") {
			command.assign(argv + i + 1, argv + argc);
			if (command.empty()) {
				std::fprintf(stderr, "-- requires a command\n");
				return 2;
			}
			break;
		}
	}

	Pty pty;
	std::mutex mutex; // Guards `host`: fed on the reader thread, inspected on main.
	TerminalHost host([&](const std::string& bytes) { pty.write(bytes); });
	host.session.onLog = [](const tsp::LogEntry& e) {
		std::printf("%s %s %6zu  %s\n", e.out ? "->" : "<-", e.verb.c_str(), e.bytes, e.text.c_str());
		std::fflush(stdout);
	};
	std::atomic<bool> exited{false};
	std::string err;
	bool started = pty.start(
	    command, "", host.session.cols, 40,
	    [&](std::string_view bytes) {
		    std::lock_guard lock(mutex);
		    host.feed(bytes);
		    host.session.flushAcks();
	    },
	    [&](int code) {
		    std::printf("== child exited with %d\n", code);
		    exited = true;
	    },
	    err);
	if (!started) {
		std::fprintf(stderr, "spawn failed: %s\n", err.c_str());
		return 2;
	}
	std::printf("== pseudo-console: %.*s; replies: %.*s\n", static_cast<int>(pty.backend().size()),
	            pty.backend().data(), static_cast<int>(replyFraming.size()), replyFraming.data());

	const auto begin = std::chrono::steady_clock::now();
	bool typedSent = typed.empty();
	while (!exited && std::chrono::steady_clock::now() - begin < std::chrono::duration<double>(seconds)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!typedSent) {
			std::lock_guard lock(mutex);
			bool surfaceLive = false;
			for (auto& s : host.session.surfaces())
				surfaceLive = surfaceLive || s->open;
			if (surfaceLive && std::chrono::steady_clock::now() - begin > std::chrono::seconds(4)) {
				std::printf("== typing %zu bytes\n", typed.size());
				pty.write(typed);
				typedSent = true;
			}
		}
	}

	int status = 1;
	{
		std::lock_guard lock(mutex);
		auto& s = host.session;
		std::printf("== hello answered: %s (program %s %s)\n", s.helloAnswered() ? "yes" : "no",
		            s.programName().c_str(), s.programVersion().c_str());
		std::printf("== frames %llu, ops %llu, rejected ops %llu, win32-input-mode %s\n",
		            static_cast<unsigned long long>(s.framesApplied), static_cast<unsigned long long>(s.opsApplied),
		            static_cast<unsigned long long>(s.opErrors), host.win32InputMode ? "on" : "off");
		if (!s.lastError.empty())
			std::printf("== last error: %s\n", s.lastError.c_str());
		for (auto& surface : s.surfaces())
			std::printf("== surface %s mode=%s open=%d nodes=%zu\n", surface->id.c_str(), surface->mode.c_str(),
			            surface->open, surface->doc->size());
		if (!dumpPath.empty() && !s.surfaces().empty()) {
			std::ofstream(dumpPath) << s.surfaces().back()->doc->snapshot().dump(1);
			std::printf("== document written to %s\n", dumpPath.c_str());
		}
		if (s.surfaces().empty()) {
			std::printf(
			    "== no surface; non-TSP output tail:\n%s\n",
			    s.helloAnswered()
			        ? ""
			        : host.rowsText.substr(host.rowsText.size() > 2000 ? host.rowsText.size() - 2000 : 0).c_str());
		}
		status = s.helloAnswered() && !s.surfaces().empty() && s.framesApplied > 0 && s.opErrors == 0 ? 0 : 1;
	}
	pty.close();
	return status;
}
