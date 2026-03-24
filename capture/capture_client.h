#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <httplib.h>
#include "capture/capture_config.h"

enum class sse_state { disconnected, connecting, connected, error };

enum class server_event_type { connected, disconnected, error, capture_done };

struct server_event {
    server_event_type type;
    std::string       path;    // capture_done only
    std::string       message; // error only
};

// ---------------------------------------------------------------------------
// capture_client
//
// Single worker thread.  All commands are processed in the worker loop.
// The SSE content callback only parses events — no command logic inside it.
//
// connect()       → push cmd::connect  → worker opens SSE GET (blocking)
// start_capture() → push cmd::start   → sse_cli.stop() interrupts GET
//                                        worker exits GET, executes POST /start,
//                                        then reconnects SSE
// stop_capture()  → push cmd::stop    → same as start (interrupt → POST → reconnect)
// disconnect()    → push cmd::disconnect → interrupt → POST /disconnect → exit loop
// ---------------------------------------------------------------------------
class capture_client {
public:
    explicit capture_client(capture_config cfg);
    ~capture_client();

    capture_client(const capture_client&)            = delete;
    capture_client& operator=(const capture_client&) = delete;

    void connect();
    void disconnect();
    void start_capture();
    void stop_capture();

    std::optional<server_event> poll_server_event();

    sse_state   get_sse_state()  const { return sse_state_.load(); }
    std::string get_last_error() const;

private:
    enum class cmd { connect, start_capture, stop_capture, disconnect };

    void worker_thread_func();
    void run_sse(bool& disconnect_requested);

    bool do_connect_post();
    bool do_disconnect_post();
    bool do_start_post();
    bool do_stop_post();

    void push_cmd(cmd c);
    void interrupt_sse();   // stop SSE + notify cv
    void dispatch_event(const std::string& event_type, const std::string& data);
    void push_event(server_event ev);
    void set_error(std::string msg);

    capture_config cfg_;

    std::thread            worker_thread_;

    std::mutex       sse_cli_mtx_;
    httplib::Client* sse_cli_ptr_{nullptr};

    std::mutex              cmd_mtx_;
    std::condition_variable cmd_cv_;
    std::deque<cmd>         cmd_queue_;
    bool                    shutdown_{false};

    std::atomic<sse_state> sse_state_{sse_state::disconnected};

    mutable std::mutex        event_mtx_;
    std::vector<server_event> event_queue_;

    mutable std::mutex error_mtx_;
    std::string        last_error_;
};
