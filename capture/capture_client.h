#pragma once
#include <atomic>
#include <deque>
#include <memory>
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
// All HTTP communication runs on a single background worker thread.
// The UI thread only enqueues commands and polls results.
// ---------------------------------------------------------------------------
class capture_client {
public:
    explicit capture_client(capture_config cfg);
    ~capture_client();

    capture_client(const capture_client&)            = delete;
    capture_client& operator=(const capture_client&) = delete;

    // Start the worker thread.
    // The thread opens GET /events (SSE) then PUTs /connect on the same thread.
    void connect();

    // Enqueue a disconnect.  The worker posts /disconnect then exits.
    void disconnect();

    // Enqueue start / stop capture commands.  Fire-and-forget from UI thread.
    // Errors are reported back via the event queue as server_event_type::error.
    void start_capture();
    void stop_capture();

    // Call from the main thread every frame.
    std::optional<server_event> poll_server_event();

    sse_state   get_sse_state()  const { return sse_state_.load(); }
    std::string get_last_error() const;

private:
    enum class cmd { disconnect, start_capture, stop_capture };

    void worker_thread_func();

    // HTTP helpers (all called from worker thread only).
    bool do_connect_post();
    bool do_disconnect_post();
    bool do_start_post();
    bool do_stop_post();

    void push_cmd(cmd c);
    bool pop_cmd(cmd& out);       // returns false if queue empty

    void dispatch_event(const std::string& event_type, const std::string& data);
    void push_event(server_event ev);
    void set_error(std::string msg);

    capture_config            cfg_;

    std::thread               worker_thread_;
    std::atomic<bool>         stop_flag_{false};
    std::atomic<sse_state>    sse_state_{sse_state::disconnected};

    // Raw pointer to the SSE client — set once before Get(), cleared after.
    // disconnect() calls stop() on it to interrupt the blocking Get()
    // from the UI thread. httplib::Client::stop() is thread-safe.
    std::mutex                sse_cli_mtx_;
    httplib::Client*          sse_cli_ptr_{nullptr};

    mutable std::mutex        cmd_mtx_;
    std::deque<cmd>           cmd_queue_;

    mutable std::mutex        event_mtx_;
    std::vector<server_event> event_queue_;

    mutable std::mutex        error_mtx_;
    std::string               last_error_;
};
