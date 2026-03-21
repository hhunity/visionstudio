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
    if (sse_state_.load() != sse_state::error)
        sse_state_.store(sse_state::disconnected);
}

void capture_client::sse_thread_func() {
    sse_state_.store(sse_state::connecting);

    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_read_timeout(0, 0);  // no timeout for streaming

    std::string buf;
    std::string current_event;
    std::string current_data;

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
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                if (line.empty()) {
                    // Blank line: dispatch accumulated event
                    if (!current_event.empty() || !current_data.empty()) {
                        dispatch_event(current_event, current_data);
                        current_event.clear();
                        current_data.clear();
                    }
                } else if (line.rfind("event:", 0) == 0) {
                    current_event = trim(line.substr(6));
                } else if (line.rfind("data:", 0) == 0) {
                    current_data = trim(line.substr(5));
                }
                // Ignore id:, retry:, and comment lines
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

void capture_client::dispatch_event(const std::string& event_type,
                                    const std::string& data) {
    if (event_type == "connected") {
        push_event({server_event_type::connected, {}, {}});
    } else if (event_type == "disconnected") {
        push_event({server_event_type::disconnected, {}, {}});
    } else if (event_type == "error") {
        std::string msg;
        try {
            const auto j = nlohmann::json::parse(data);
            if (j.contains("message") && j["message"].is_string())
                msg = j["message"].get<std::string>();
        } catch (...) {}
        push_event({server_event_type::error, {}, msg});
    } else if (event_type == "capture_done") {
        std::string path;
        try {
            const auto j = nlohmann::json::parse(data);
            if (j.contains("path") && j["path"].is_string())
                path = j["path"].get<std::string>();
        } catch (...) {}
        if (!path.empty())
            push_event({server_event_type::capture_done, path, {}});
    }
}

// ---------------------------------------------------------------------------
// Thread-safe event queue / error helpers
// ---------------------------------------------------------------------------

void capture_client::push_event(server_event ev) {
    std::lock_guard lock(event_mtx_);
    event_queue_.push_back(std::move(ev));
}

std::optional<server_event> capture_client::poll_server_event() {
    std::lock_guard lock(event_mtx_);
    if (event_queue_.empty()) return std::nullopt;
    server_event ev = std::move(event_queue_.front());
    event_queue_.erase(event_queue_.begin());
    return ev;
}

void capture_client::set_error(std::string msg) {
    std::lock_guard lock(error_mtx_);
    last_error_ = std::move(msg);
}

std::string capture_client::get_last_error() const {
    std::lock_guard lock(error_mtx_);
    return last_error_;
}
