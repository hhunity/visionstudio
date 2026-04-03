// Sandbox CLI for capture_client.
// Exercises the real capture_client library so you can test server communication
// without launching the full GUI.
//
// Usage:
//   capture_cli [--host HOST] [--port PORT] [command ...]
//
// Batch mode (commands as arguments):
//   capture_cli connect start wait 3000 stop disconnect
//   capture_cli --port 9090 connect
//   capture_cli connect preview_start wait 5000 preview_stop disconnect
//
// Interactive mode (no commands given):
//   capture_cli
//   capture_cli --host 192.168.1.10 --port 9090
//
// Commands:
//   connect        - connect() + wait for SSE connected
//   start          - start_capture()
//   stop           - stop_capture()
//   disconnect     - disconnect()
//   wait <ms>      - sleep for N milliseconds (polling events + preview frames during wait)
//   status         - print current SSE state and preview active flag
//   preview_start  - start MJPEG preview stream
//   preview_stop   - stop MJPEG preview stream
//   quit / exit    - exit (interactive mode only)

#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "capture/capture_client.h"
#include "util/capture_config.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* sse_state_str(sse_state s) {
    switch (s) {
    case sse_state::disconnected: return "disconnected";
    case sse_state::connecting:   return "connecting";
    case sse_state::connected:    return "connected";
    case sse_state::error:        return "error";
    }
    return "?";
}

// Drain all queued events, update local state, and print them.
// Returns true if a terminal event (disconnected / error) arrived.
static bool drain_events(capture_client& client, sse_state& state) {
    bool done = false;
    while (auto ev = client.poll_server_event()) {
        if (std::get_if<evt_connected>(&*ev)) {
            state = sse_state::connected;
            std::cout << "[event] connected\n";
        } else if (std::get_if<evt_disconnected>(&*ev)) {
            state = sse_state::disconnected;
            std::cout << "[event] disconnected\n";
            done = true;
        } else if (auto* e = std::get_if<evt_error>(&*ev)) {
            state = sse_state::error;
            std::cout << "[event] error: " << e->message << "\n";
            done = true;
        } else if (auto* e = std::get_if<evt_capture_done>(&*ev)) {
            std::cout << "[event] capture_done  path=" << e->path << "\n";
        }
    }
    return done;
}

// Wait until connected event arrives (or error/timeout).
static bool wait_connected(capture_client& client, sse_state& state,
                           int timeout_ms = 10000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        drain_events(client, state);
        if (state == sse_state::connected) return true;
        if (state == sse_state::error)     return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cerr << "timeout waiting for connected\n";
    return false;
}

// Execute a single parsed command.  Returns false if the caller should exit.
static bool exec_cmd(const std::vector<std::string>& tokens, size_t& i,
                     capture_client& client, sse_state& state) {
    if (tokens.empty()) return true;
    const auto& cmd = tokens[i];

    if (cmd == "quit" || cmd == "exit") {
        return false;

    } else if (cmd == "connect") {
        std::cout << ">> connect\n";
        client.connect();
        wait_connected(client, state);

    } else if (cmd == "start") {
        std::cout << ">> start_capture\n";
        client.start_capture();
        drain_events(client, state);

    } else if (cmd == "stop") {
        std::cout << ">> stop_capture\n";
        client.stop_capture();
        drain_events(client, state);

    } else if (cmd == "disconnect") {
        std::cout << ">> disconnect\n";
        client.disconnect();
        for (int t = 0; t < 100; ++t) {
            if (drain_events(client, state)) break;
            if (state == sse_state::disconnected || state == sse_state::error) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

    } else if (cmd == "wait") {
        if (i + 1 >= tokens.size()) {
            std::cerr << "wait requires a millisecond argument\n";
            return true;
        }
        const int ms = std::stoi(tokens[++i]);
        std::cout << ">> wait " << ms << "ms\n";
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        int frame_count = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            drain_events(client, state);
            preview_frame frame;
            while (client.poll_preview_frame(frame)) {
                ++frame_count;
                std::cout << "[preview] frame #" << frame_count
                          << "  " << frame.w << "x" << frame.h
                          << "  (" << frame.pixels.size() << " bytes)\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (frame_count > 0)
            std::cout << "[preview] total frames received: " << frame_count << "\n";

    } else if (cmd == "status") {
        std::cout << "SSE state:      " << sse_state_str(state) << "\n";
        std::cout << "preview active: " << (client.is_preview_active() ? "yes" : "no") << "\n";

    } else if (cmd == "preview_start") {
        std::cout << ">> preview_start\n";
        client.start_preview();

    } else if (cmd == "preview_stop") {
        std::cout << ">> preview_stop\n";
        client.stop_preview();

    } else {
        std::cerr << "unknown command: " << cmd
                  << "  (connect / start / stop / disconnect / wait <ms> / status"
                     " / preview_start / preview_stop / quit)\n";
    }
    return true;
}

// ---------------------------------------------------------------------------
// Interactive REPL
// ---------------------------------------------------------------------------

static void run_interactive(capture_client& client, sse_state& state) {
    std::cout << "capture_cli interactive mode\n"
              << "commands: connect  start  stop  disconnect  wait <ms>  status"
                 "  preview_start  preview_stop  quit\n\n";

    // Background thread: poll events + preview frames while user is typing
    std::atomic<bool> stop_poller{false};
    std::atomic<int>  total_preview_frames{0};
    std::mutex        state_mtx;
    auto poll_fn = [&] {
        while (!stop_poller.load()) {
            { std::lock_guard lock(state_mtx); drain_events(client, state); }
            preview_frame frame;
            while (client.poll_preview_frame(frame)) {
                const int n = ++total_preview_frames;
                std::cout << "\n[preview] frame #" << n
                          << "  " << frame.w << "x" << frame.h
                          << "  (" << frame.pixels.size() << " bytes)\n> " << std::flush;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };
    std::thread poller(poll_fn);

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        std::istringstream iss(line);
        std::vector<std::string> tokens;
        std::string tok;
        while (iss >> tok) tokens.push_back(tok);
        if (tokens.empty()) continue;

        // Pause the background poller so output doesn't interleave with command execution
        stop_poller.store(true);
        poller.join();

        size_t i = 0;
        bool keep_going = true;
        for (i = 0; i < tokens.size() && keep_going; ++i)
            keep_going = exec_cmd(tokens, i, client, state);

        if (!keep_going) break;

        // Restart background poller
        stop_poller.store(false);
        poller = std::thread(poll_fn);
    }

    stop_poller.store(true);
    if (poller.joinable()) poller.join();
    std::cout << "bye\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    CLI::App app{"capture_cli - sandbox for capture_client library"};

    std::string host = "localhost";
    int         port = 8080;
    std::vector<std::string> cmds;

    app.add_option("--host", host, "Server host")->default_str("localhost");
    app.add_option("--port", port, "Server port")->default_str("8080");
    app.add_option("commands", cmds,
                   "Commands to run in batch mode. Omit for interactive mode.");

    CLI11_PARSE(app, argc, argv);

    capture_config cfg;
    cfg.host = host;
    cfg.port = port;

    capture_client client(cfg);
    client.set_logger([](const std::string& msg) {
        std::cout << msg << "\n";
    });

    sse_state state = sse_state::disconnected;

    if (cmds.empty()) {
        run_interactive(client, state);
    } else {
        for (size_t i = 0; i < cmds.size(); ++i)
            exec_cmd(cmds, i, client, state);
    }

    return 0;
}
