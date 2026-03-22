#include "capture/capture_client.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
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
    if (sse_thread_.joinable()) {
        const auto s = sse_state_.load();
        if (s == sse_state::connecting || s == sse_state::connected)
            return; // actively running
        sse_thread_.join(); // clean up finished thread before restart
    }
    stop_flag_.store(false);
    sse_state_.store(sse_state::connecting); // set before thread starts to avoid race with error detection
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
    // Do not set read_timeout to (0,0): that means poll(fd,1,0) = non-blocking,
    // which immediately fails when no SSE body data is available.
    // Use a large timeout; keepalives arrive every ~15s so it never fires.
    cli.set_read_timeout(3600, 0);  // 1 hour

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
        if (stop_flag_.load()) {
            // Intentional stop via stop_sse(); let stop_sse() set disconnected.
        } else {
            set_error("SSE: " + httplib::to_string(res.error()));
            sse_state_.store(sse_state::error);
        }
    } else if (sse_state_.load() == sse_state::connected) {
        sse_state_.store(sse_state::disconnected);
    }
}

void capture_client::dispatch_event(const std::string& event_type,
                                    const std::string& data) {
    if (event_type == "disconnected") {
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
        std::string url;
        try {
            const auto j = nlohmann::json::parse(data);
            if (j.contains("url") && j["url"].is_string())
                url = j["url"].get<std::string>();
        } catch (...) {}
        if (!url.empty()) {
            const auto local_path = download_capture(url);
            if (!local_path.empty())
                push_event({server_event_type::capture_done, local_path, {}});
            else
                push_event({server_event_type::error, {}, "capture download failed: " + url});
        }
    }
}

// ---------------------------------------------------------------------------
// TIFF download helper
// ---------------------------------------------------------------------------

std::string capture_client::download_capture(const std::string& url) {
    // Extract path component from "http://host:port/path"
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {};
    const auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return {};
    const std::string path = url.substr(path_start);

    // Build a unique temp file path
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("vs_capture_" + std::to_string(now) + ".tiff");

    std::ofstream f(tmp, std::ios::binary);
    if (!f) return {};

    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_read_timeout(cfg_.timeout_ms / 1000, (cfg_.timeout_ms % 1000) * 1000);
    const auto res = cli.Get(path, [&](const char* data, size_t len) {
        f.write(data, len);
        return true;
    });
    f.close();

    if (!res || res->status != 200) {
        std::filesystem::remove(tmp);
        return {};
    }
    return tmp.string();
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
