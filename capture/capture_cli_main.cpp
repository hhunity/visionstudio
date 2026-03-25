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
//
// Interactive mode (no commands given):
//   capture_cli
//   capture_cli --host 192.168.1.10 --port 9090
//
// Commands:
//   connect      - connect() + wait for SSE connected
//   start        - start_capture()
//   stop         - stop_capture()
//   disconnect   - disconnect()
//   wait <ms>    - sleep for N milliseconds (polling events during wait)
//   status       - print current SSE state
//   quit / exit  - exit (interactive mode only)

#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "capture/capture_client.h"
#include "capture/capture_config.h"

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

// Drain all queued events and print them.  Returns true if a terminal event
// (disconnected / error) arrived.
static bool drain_events(capture_client& client) {
    bool done = false;
    while (auto ev = client.poll_server_event()) {
        if (auto* e = std::get_if<evt_error>(&*ev)) {
            std::cout << "[event] error: " << e->message << "\n";
            done = true;
        } else if (auto* e = std::get_if<evt_capture_done>(&*ev)) {
            std::cout << "[event] capture_done  path=" << e->path << "\n";
        }
    }
    return done;
}

// Wait until sse_state reaches connected (or error/timeout).
static bool wait_connected(capture_client& client, int timeout_ms = 10000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        drain_events(client);
        const auto state = client.get_sse_state();
        if (state == sse_state::connected) return true;
        if (state == sse_state::error) {
            std::cerr << "error: " << client.get_last_error() << "\n";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cerr << "timeout waiting for connected\n";
    return false;
}

// Execute a single parsed command.  Returns false if the caller should exit.
static bool exec_cmd(const std::vector<std::string>& tokens, size_t& i,
                     capture_client& client) {
    if (tokens.empty()) return true;
    const auto& cmd = tokens[i];

    if (cmd == "quit" || cmd == "exit") {
        return false;

    } else if (cmd == "connect") {
        std::cout << ">> connect\n";
        client.connect();
        wait_connected(client);

    } else if (cmd == "start") {
        std::cout << ">> start_capture\n";
        client.start_capture();
        drain_events(client);

    } else if (cmd == "stop") {
        std::cout << ">> stop_capture\n";
        client.stop_capture();
        drain_events(client);

    } else if (cmd == "disconnect") {
        std::cout << ">> disconnect\n";
        client.disconnect();
        for (int t = 0; t < 100; ++t) {
            if (drain_events(client)) break;
            const auto s = client.get_sse_state();
            if (s == sse_state::disconnected || s == sse_state::error) break;
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
        while (std::chrono::steady_clock::now() < deadline) {
            drain_events(client);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

    } else if (cmd == "status") {
        std::cout << "SSE state: " << sse_state_str(client.get_sse_state()) << "\n";
        const auto err = client.get_last_error();
        if (!err.empty()) std::cout << "last error: " << err << "\n";

    } else {
        std::cerr << "unknown command: " << cmd
                  << "  (connect / start / stop / disconnect / wait <ms> / status / quit)\n";
    }
    return true;
}

// ---------------------------------------------------------------------------
// Interactive REPL
// ---------------------------------------------------------------------------

static void run_interactive(capture_client& client) {
    std::cout << "capture_cli interactive mode\n"
              << "commands: connect  start  stop  disconnect  wait <ms>  status  quit\n\n";

    // Background thread: poll events while user is typing
    std::atomic<bool> stop_poller{false};
    std::thread poller([&] {
        while (!stop_poller.load()) {
            drain_events(client);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

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
            keep_going = exec_cmd(tokens, i, client);

        if (!keep_going) break;

        // Restart background poller
        stop_poller.store(false);
        poller = std::thread([&] {
            while (!stop_poller.load()) {
                drain_events(client);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
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

    if (cmds.empty()) {
        run_interactive(client);
    } else {
        for (size_t i = 0; i < cmds.size(); ++i)
            exec_cmd(cmds, i, client);
    }

    return 0;
}
