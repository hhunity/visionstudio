#pragma once
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "capture/capture_config.h"

enum class sse_state { disconnected, connecting, connected, error };

enum class server_event_type { connected, disconnected, error, capture_done };

struct server_event {
    server_event_type type;
    std::string       path;    // capture_done only
    std::string       message; // error only
};

class capture_client {
public:
    explicit capture_client(capture_config cfg);
    ~capture_client();

    capture_client(const capture_client&)            = delete;
    capture_client& operator=(const capture_client&) = delete;

    // Open SSE stream, then POST /connect on the same thread once the stream
    // is confirmed up.  Non-blocking: spawns background thread and returns.
    void connect();

    // POST /disconnect, then join the SSE thread.  Returns true on HTTP 2xx.
    bool disconnect();

    // Send POST to start / stop endpoint. Returns true on HTTP 2xx.
    bool start_capture();
    bool stop_capture();

    // Call from the main thread every frame.
    // Returns the next server event if one is queued, or nullopt.
    std::optional<server_event> poll_server_event();

    sse_state   get_sse_state()  const { return sse_state_.load(); }
    std::string get_last_error() const;

private:
    void sse_thread_func();
    bool do_connect_post();
    bool do_disconnect_post();
    void stop_sse();
    void dispatch_event(const std::string& event_type, const std::string& data);
    void push_event(server_event ev);
    void set_error(std::string msg);

    capture_config            cfg_;

    std::thread               sse_thread_;
    std::atomic<bool>         stop_flag_{false};
    std::atomic<sse_state>    sse_state_{sse_state::disconnected};

    mutable std::mutex        event_mtx_;
    std::vector<server_event> event_queue_;

    mutable std::mutex        error_mtx_;
    std::string               last_error_;
};
