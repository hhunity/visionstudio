#include "capture/capture_client.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sstream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// ---------------------------------------------------------------------------
// capture_client
// ---------------------------------------------------------------------------

capture_client::capture_client(capture_config cfg)
    : cfg_(std::move(cfg)) {}

capture_client::~capture_client() {
    stop_sse();
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

bool capture_client::connect_server() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_connection_timeout(cfg_.timeout_ms / 1000,
                               (cfg_.timeout_ms % 1000) * 1000);
    cli.set_read_timeout(cfg_.timeout_ms / 1000,
                         (cfg_.timeout_ms % 1000) * 1000);
    auto res = cli.Post(cfg_.connect_path);
    if (!res) {
        set_error("connect_server: connection failed");
        return false;
    }
    if (res->status < 200 || res->status >= 300) {
        set_error("connect_server: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

bool capture_client::disconnect_server() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_connection_timeout(cfg_.timeout_ms / 1000,
                               (cfg_.timeout_ms % 1000) * 1000);
    cli.set_read_timeout(cfg_.timeout_ms / 1000,
                         (cfg_.timeout_ms % 1000) * 1000);
    auto res = cli.Post(cfg_.disconnect_path);
    if (!res) {
        set_error("disconnect_server: connection failed");
        return false;
    }
    if (res->status < 200 || res->status >= 300) {
        set_error("disconnect_server: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

bool capture_client::start_capture() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_connection_timeout(cfg_.timeout_ms / 1000,
                               (cfg_.timeout_ms % 1000) * 1000);
    cli.set_read_timeout(cfg_.timeout_ms / 1000,
                         (cfg_.timeout_ms % 1000) * 1000);
    auto res = cli.Post(cfg_.start_path);
    if (!res) {
        set_error("start_capture: connection failed");
        return false;
    }
    if (res->status < 200 || res->status >= 300) {
        set_error("start_capture: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

bool capture_client::stop_capture() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_connection_timeout(cfg_.timeout_ms / 1000,
                               (cfg_.timeout_ms % 1000) * 1000);
    cli.set_read_timeout(cfg_.timeout_ms / 1000,
                         (cfg_.timeout_ms % 1000) * 1000);
    auto res = cli.Post(cfg_.stop_path);
    if (!res) {
        set_error("stop_capture: connection failed");
        return false;
    }
    if (res->status < 200 || res->status >= 300) {
        set_error("stop_capture: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------

void capture_client::start_sse() {
    if (sse_thread_.joinable()) return;  // already running
    stop_flag_.store(false);
    sse_thread_ = std::thread(&capture_client::sse_thread_func, this);
}

void capture_client::stop_sse() {
    stop_flag_.store(true);
    if (sse_thread_.joinable()) sse_thread_.join();
    sse_state_.store(sse_state::disconnected);
}

void capture_client::sse_thread_func() {
    sse_state_.store(sse_state::connecting);

    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_read_timeout(0, 0);  // no timeout for streaming

    std::string buf;

    auto res = cli.Get(
        cfg_.sse_path,
        httplib::Headers{{"Accept", "text/event-stream"},
                         {"Cache-Control", "no-cache"}},
        [&](const httplib::Response& response) {
            if (response.status < 200 || response.status >= 300) {
                set_error("SSE: HTTP " + std::to_string(response.status));
                sse_state_.store(sse_state::error);
                return false;
            }
            sse_state_.store(sse_state::connected);
            return true;
        },
        [&](const char* data, size_t len) -> bool {
            buf.append(data, len);
            // Process all complete lines ('\n' delimited).
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                parse_sse_line(line);
            }
            return !stop_flag_.load();
        });

    if (!res && sse_state_.load() != sse_state::error) {
        set_error("SSE: connection failed");
        sse_state_.store(sse_state::error);
    } else if (sse_state_.load() == sse_state::connected) {
        sse_state_.store(sse_state::disconnected);
    }
}

void capture_client::parse_sse_line(const std::string& line) {
    // SSE format: "data: <json>"
    if (line.rfind("data:", 0) != 0) return;
    const std::string json_str = trim(line.substr(5));
    if (json_str.empty()) return;

    try {
        const auto j = nlohmann::json::parse(json_str);
        if (j.contains("path") && j["path"].is_string())
            push_result(j["path"].get<std::string>());
    } catch (...) {
        // Ignore malformed JSON lines.
    }
}

// ---------------------------------------------------------------------------
// Thread-safe queue / error helpers
// ---------------------------------------------------------------------------

void capture_client::push_result(std::string path) {
    std::lock_guard lock(queue_mtx_);
    result_queue_.push_back(std::move(path));
}

std::optional<std::string> capture_client::poll_result() {
    std::lock_guard lock(queue_mtx_);
    if (result_queue_.empty()) return std::nullopt;
    std::string path = std::move(result_queue_.front());
    result_queue_.erase(result_queue_.begin());
    return path;
}

void capture_client::set_error(std::string msg) {
    std::lock_guard lock(error_mtx_);
    last_error_ = std::move(msg);
}

std::string capture_client::get_last_error() const {
    std::lock_guard lock(error_mtx_);
    return last_error_;
}
