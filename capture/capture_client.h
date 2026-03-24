#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <httplib.h>
#include "capture/capture_config.h"

enum class sse_state { disconnected, connecting, connected, error };

enum class server_event_type { disconnected, error, capture_done };

struct server_event {
    server_event_type type;
    std::string       path;    // capture_done only
    std::string       message; // error only
};

class capture_client {
public:
    using logger_fn = std::function<void(const std::string&)>;

    explicit capture_client(capture_config cfg);
    ~capture_client();

    capture_client(const capture_client&)            = delete;
    capture_client& operator=(const capture_client&) = delete;

    void connect();
    void disconnect();
    void start_capture();
    void stop_capture();

    void set_logger(logger_fn fn);

    std::optional<server_event> poll_server_event();

    sse_state   get_sse_state()  const { return sse_state_.load(); }
    std::string get_last_error() const;

private:
    enum class cmd { connect, start_capture, stop_capture, disconnect };

    void worker_thread_func();
    void run_sse();

    httplib::Client make_cli() const;
    bool do_connect_post();
    bool do_simple_post(const std::string& path, const std::string& label);

    void push_cmd(cmd c);
    void interrupt_sse();
    void dispatch_event(const std::string& event_type, const std::string& data);
    void push_event(server_event ev);
    void set_error(std::string msg);
    void log(const std::string& msg) const;

    capture_config cfg_;

    std::thread worker_thread_;
    std::thread sse_thread_;

    std::mutex       sse_cli_mtx_;
    httplib::Client* sse_cli_ptr_{nullptr};

    std::mutex              sse_ready_mtx_;
    std::condition_variable sse_ready_cv_;
    bool                    sse_ready_{false};
    bool                    sse_ready_ok_{false};

    std::mutex              cmd_mtx_;
    std::condition_variable cmd_cv_;
    std::deque<cmd>         cmd_queue_;
    bool                    shutdown_{false};

    std::atomic<sse_state> sse_state_{sse_state::disconnected};

    mutable std::mutex  event_mtx_;
    std::deque<server_event> event_queue_;

    mutable std::mutex error_mtx_;
    std::string        last_error_;

    mutable std::mutex logger_mtx_;
    logger_fn          logger_;
};
