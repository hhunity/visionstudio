#pragma once
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "capture/capture_config.h"

enum class sse_state { disconnected, connecting, connected, error };

class capture_client {
public:
    explicit capture_client(capture_config cfg);
    ~capture_client();

    capture_client(const capture_client&)            = delete;
    capture_client& operator=(const capture_client&) = delete;

    // Send POST to start / stop endpoint. Returns true on HTTP 2xx.
    bool start_capture();
    bool stop_capture();

    // Start the SSE listener thread. No-op if already running.
    void start_sse();
    // Stop the SSE listener thread. Safe to call multiple times.
    void stop_sse();

    // Call from the main thread every frame.
    // Returns the next completed image path if one is queued, or nullopt.
    std::optional<std::string> poll_result();

    sse_state   get_sse_state()  const { return sse_state_.load(); }
    std::string get_last_error() const;

private:
    void sse_thread_func();
    void parse_sse_line(const std::string& line);
    void push_result(std::string path);
    void set_error(std::string msg);

    capture_config           cfg_;

    std::thread              sse_thread_;
    std::atomic<bool>        stop_flag_{false};
    std::atomic<sse_state>   sse_state_{sse_state::disconnected};

    mutable std::mutex       queue_mtx_;
    std::vector<std::string> result_queue_;

    mutable std::mutex       error_mtx_;
    std::string              last_error_;
};
