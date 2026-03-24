// Sandbox CLI for capture_client.
// Exercises the real capture_client library so you can test server communication
// without launching the full GUI.
//
// Usage:
//   capture_cli [--host HOST] [--port PORT] <command>
//
// Commands (executed in order, multiple allowed):
//   connect      - connect() + wait for SSE connected
//   start        - start_capture()
//   stop         - stop_capture()
//   disconnect   - disconnect()
//   wait <ms>    - sleep for N milliseconds
//
// Examples:
//   capture_cli connect start wait 3000 stop disconnect
//   capture_cli --port 9090 connect

#include <CLI/CLI.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "capture/capture_client.h"
#include "capture/capture_config.h"

// Poll events from the client and print them.  Returns true if a "done" event
// (disconnected / error) arrived.
static bool drain_events(capture_client& client) {
    bool done = false;
    while (auto ev = client.poll_server_event()) {
        switch (ev->type) {
        case server_event_type::connected:
            std::cout << "[event] connected\n";
            break;
        case server_event_type::disconnected:
            std::cout << "[event] disconnected\n";
            done = true;
            break;
        case server_event_type::error:
            std::cout << "[event] error: " << ev->message << "\n";
            done = true;
            break;
        case server_event_type::capture_done:
            std::cout << "[event] capture_done  path=" << ev->path << "\n";
            break;
        }
    }
    return done;
}

// Wait until sse_state reaches connected (or error), printing events as they arrive.
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

int main(int argc, char** argv) {
    CLI::App app{"capture_cli - sandbox for capture_client library"};

    std::string host = "localhost";
    int         port = 8080;
    std::vector<std::string> cmds;

    app.add_option("--host", host, "Server host")->default_str("localhost");
    app.add_option("--port", port, "Server port")->default_str("8080");
    app.add_option("commands", cmds, "Commands: connect start stop disconnect wait <ms>")
        ->required();

    CLI11_PARSE(app, argc, argv);

    capture_config cfg;
    cfg.host = host;
    cfg.port = port;

    capture_client client(cfg);
    client.set_logger([](const std::string& msg) {
        std::cout << msg << "\n";
    });

    for (size_t i = 0; i < cmds.size(); ++i) {
        const auto& cmd = cmds[i];

        if (cmd == "connect") {
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
            // drain until disconnected or error
            for (int t = 0; t < 100; ++t) {
                if (drain_events(client)) break;
                const auto s = client.get_sse_state();
                if (s == sse_state::disconnected || s == sse_state::error) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

        } else if (cmd == "wait") {
            if (i + 1 >= cmds.size()) {
                std::cerr << "wait requires a millisecond argument\n";
                return 1;
            }
            const int ms = std::stoi(cmds[++i]);
            std::cout << ">> wait " << ms << "ms\n";
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < deadline) {
                drain_events(client);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

        } else {
            std::cerr << "unknown command: " << cmd << "\n";
            return 1;
        }
    }

    return 0;
}
