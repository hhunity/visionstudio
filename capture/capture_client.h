#pragma once
#include <atomic>
#include <condition_variable>
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
// Two background threads, UI thread never blocks on HTTP:
//
//   sse_thread   GET /events + PUT /connect (response callback)
//                SSE parsing only — no command processing
//
//   cmd_thread   start / stop / disconnect via condition variable queue
//                POST /disconnect before signalling sse_thread to stop
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
    enum class cmd { start_capture, stop_capture, disconnect };

    void sse_thread_func();
    void cmd_thread_func();

    bool do_connect_post();
    bool do_disconnect_post();
    bool do_start_post();
    bool do_stop_post();

    void push_cmd(cmd c);
    void dispatch_event(const std::string& event_type, const std::string& data);
    void push_event(server_event ev);
    void set_error(std::string msg);

    capture_config cfg_;

    std::thread            sse_thread_;
    std::thread            cmd_thread_;
    std::atomic<bool>      stop_flag_{false};
    std::atomic<sse_state> sse_state_{sse_state::disconnected};

    // sse_cli_ptr_ lets disconnect() call stop() to interrupt the blocking Get.
    // httplib::Client::stop() is thread-safe.
    std::mutex       sse_cli_mtx_;
    httplib::Client* sse_cli_ptr_{nullptr};

    std::mutex              cmd_mtx_;
    std::condition_variable cmd_cv_;
    std::deque<cmd>         cmd_queue_;

    mutable std::mutex        event_mtx_;
    std::vector<server_event> event_queue_;

    mutable std::mutex error_mtx_;
    std::string        last_error_;
};
