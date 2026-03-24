#include "capture/capture_client.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

static std::string trim(const std::string& s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    auto b = std::find_if_not(s.begin(), s.end(), is_ws);
    auto e = std::find_if_not(s.rbegin(), s.rend(), is_ws).base();
    return b < e ? std::string(b, e) : std::string{};
}

// ---------------------------------------------------------------------------
// capture_client
// ---------------------------------------------------------------------------

capture_client::capture_client(capture_config cfg) : cfg_(std::move(cfg)) {
    worker_thread_ = std::thread(&capture_client::worker_thread_func, this);
}

capture_client::~capture_client() {
    {
        std::lock_guard lock(cmd_mtx_);
        shutdown_ = true;
    }
    cmd_cv_.notify_all();
    {
        std::lock_guard lock(sse_cli_mtx_);
        if (sse_cli_ptr_) sse_cli_ptr_->stop();
    }
    if (worker_thread_.joinable()) worker_thread_.join();
}

// ---------------------------------------------------------------------------
// Public API  (UI thread)
// ---------------------------------------------------------------------------

void capture_client::connect() {
    push_cmd(cmd::connect);
    cmd_cv_.notify_one();
}

void capture_client::disconnect() {
    push_cmd(cmd::disconnect);
    cmd_cv_.notify_one();
    // Also interrupt a silent SSE stream immediately.
    std::lock_guard lock(sse_cli_mtx_);
    if (sse_cli_ptr_) sse_cli_ptr_->stop();
}

void capture_client::start_capture() {
    push_cmd(cmd::start_capture);
    cmd_cv_.notify_one();
}

void capture_client::stop_capture() {
    push_cmd(cmd::stop_capture);
    cmd_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Worker thread
//
// Waits for commands and processes them one by one.
// cmd::connect opens a blocking SSE GET; while streaming, start/stop/disconnect
// commands are drained inside the content callback (POST requests are fast).
// ---------------------------------------------------------------------------

void capture_client::worker_thread_func() {
    while (true) {
        cmd c;
        {
            std::unique_lock lock(cmd_mtx_);
            cmd_cv_.wait(lock, [&] {
                return !cmd_queue_.empty() || shutdown_;
            });
            if (shutdown_ && cmd_queue_.empty()) return;
            c = cmd_queue_.front();
            cmd_queue_.pop_front();
        }

        switch (c) {
        // ------------------------------------------------------------------
        case cmd::connect: {
            sse_state_.store(sse_state::connecting);

            httplib::Client sse_cli(cfg_.host, cfg_.port);
            sse_cli.set_read_timeout(0, 0);

            {
                std::lock_guard lock(sse_cli_mtx_);
                sse_cli_ptr_ = &sse_cli;
            }

            std::string buf, cur_event, cur_data;
            bool disconnect_requested = false;

            sse_cli.Get(
                cfg_.sse_path,
                httplib::Headers{{"Accept", "text/event-stream"},
                                 {"Cache-Control", "no-cache"}},

                [&](const httplib::Response& r) -> bool {
                    if (r.status < 200 || r.status >= 300) {
                        set_error("SSE: HTTP " + std::to_string(r.status));
                        sse_state_.store(sse_state::error);
                        return false;
                    }
                    if (!do_connect_post()) {
                        sse_state_.store(sse_state::error);
                        return false;
                    }
                    sse_state_.store(sse_state::connected);
                    return true;
                },

                [&](const char* data, size_t len) -> bool {
                    // Parse incoming SSE lines.
                    buf.append(data, len);
                    size_t pos;
                    while ((pos = buf.find('\n')) != std::string::npos) {
                        std::string line = buf.substr(0, pos);
                        buf.erase(0, pos + 1);
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();
                        if (line.empty()) {
                            if (!cur_event.empty() || !cur_data.empty()) {
                                dispatch_event(cur_event, cur_data);
                                cur_event.clear();
                                cur_data.clear();
                            }
                        } else if (line.rfind("event:", 0) == 0) {
                            cur_event = trim(line.substr(6));
                        } else if (line.rfind("data:", 0) == 0) {
                            cur_data = trim(line.substr(5));
                        }
                    }

                    // Drain commands that arrived while streaming.
                    // start/stop POSTs are fast; disconnect just sets a flag.
                    cmd pending;
                    while ([&] {
                        std::lock_guard lock(cmd_mtx_);
                        if (cmd_queue_.empty()) return false;
                        pending = cmd_queue_.front();
                        cmd_queue_.pop_front();
                        return true;
                    }()) {
                        switch (pending) {
                        case cmd::start_capture:
                            if (!do_start_post())
                                push_event({server_event_type::error, {},
                                            "start failed: " + get_last_error()});
                            break;
                        case cmd::stop_capture:
                            if (!do_stop_post())
                                push_event({server_event_type::error, {},
                                            "stop failed: " + get_last_error()});
                            break;
                        case cmd::disconnect:
                            disconnect_requested = true;
                            break;
                        default:
                            break;
                        }
                    }

                    return !disconnect_requested && !shutdown_;
                });

            {
                std::lock_guard lock(sse_cli_mtx_);
                sse_cli_ptr_ = nullptr;
            }

            if (disconnect_requested)
                do_disconnect_post();

            if (sse_state_.load() == sse_state::connected)
                sse_state_.store(sse_state::disconnected);
            break;
        }

        // ------------------------------------------------------------------
        // Commands that arrive before connect (or after disconnect) are ignored.
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

bool capture_client::do_connect_post() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_connection_timeout(cfg_.timeout_ms / 1000,
                               (cfg_.timeout_ms % 1000) * 1000);
    cli.set_read_timeout(cfg_.timeout_ms / 1000,
                         (cfg_.timeout_ms % 1000) * 1000);

    const nlohmann::json meta = {
        {"host",            cfg_.host},
        {"port",            cfg_.port},
        {"connect_path",    cfg_.connect_path},
        {"start_path",      cfg_.start_path},
        {"stop_path",       cfg_.stop_path},
        {"disconnect_path", cfg_.disconnect_path},
        {"sse_path",        cfg_.sse_path},
        {"timeout_ms",      cfg_.timeout_ms},
    };

    const std::string boundary = "----VisionStudioBoundary"
                                 + std::to_string(cfg_.port);
    std::string body;

    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"config\"\r\n";
    body += "Content-Type: application/json\r\n\r\n";
    body += meta.dump();
    body += "\r\n";

    for (const auto& file_path : cfg_.config_files) {
        std::ifstream f(file_path, std::ios::binary);
        if (!f.is_open()) {
            set_error("connect: cannot open config file: " + file_path);
            return false;
        }
        const std::string content((std::istreambuf_iterator<char>(f)), {});
        const auto sep   = file_path.find_last_of("/\\");
        const auto fname = sep == std::string::npos ? file_path
                                                    : file_path.substr(sep + 1);
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"camera_config\""
                "; filename=\"" + fname + "\"\r\n";
        body += "Content-Type: application/octet-stream\r\n\r\n";
        body += content;
        body += "\r\n";
    }
    body += "--" + boundary + "--\r\n";

    const std::string ct = "multipart/form-data; boundary=" + boundary;
    auto res = cli.Put(cfg_.connect_path, body, ct.c_str());
    if (!res) { set_error("connect: PUT failed"); return false; }
    if (res->status < 200 || res->status >= 300) {
        set_error("connect: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

bool capture_client::do_disconnect_post() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_connection_timeout(cfg_.timeout_ms / 1000,
                               (cfg_.timeout_ms % 1000) * 1000);
    cli.set_read_timeout(cfg_.timeout_ms / 1000,
                         (cfg_.timeout_ms % 1000) * 1000);
    auto res = cli.Post(cfg_.disconnect_path);
    if (!res) { set_error("disconnect: failed"); return false; }
    if (res->status < 200 || res->status >= 300) {
        set_error("disconnect: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

bool capture_client::do_start_post() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_connection_timeout(cfg_.timeout_ms / 1000,
                               (cfg_.timeout_ms % 1000) * 1000);
    cli.set_read_timeout(cfg_.timeout_ms / 1000,
                         (cfg_.timeout_ms % 1000) * 1000);
    auto res = cli.Post(cfg_.start_path);
    if (!res) { set_error("start: failed"); return false; }
    if (res->status < 200 || res->status >= 300) {
        set_error("start: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

bool capture_client::do_stop_post() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_connection_timeout(cfg_.timeout_ms / 1000,
                               (cfg_.timeout_ms % 1000) * 1000);
    cli.set_read_timeout(cfg_.timeout_ms / 1000,
                         (cfg_.timeout_ms % 1000) * 1000);
    auto res = cli.Post(cfg_.stop_path);
    if (!res) { set_error("stop: failed"); return false; }
    if (res->status < 200 || res->status >= 300) {
        set_error("stop: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SSE event dispatch
// ---------------------------------------------------------------------------

void capture_client::dispatch_event(const std::string& event_type,
                                    const std::string& data) {
    if (event_type == "connected") {
        push_event({server_event_type::connected, {}, {}});
    } else if (event_type == "disconnected") {
        push_event({server_event_type::disconnected, {}, {}});
    } else if (event_type == "error") {
        std::string msg;
        try {
            auto j = nlohmann::json::parse(data);
            if (j.contains("message") && j["message"].is_string())
                msg = j["message"].get<std::string>();
        } catch (...) {}
        push_event({server_event_type::error, {}, msg});
    } else if (event_type == "capture_done") {
        std::string path;
        try {
            auto j = nlohmann::json::parse(data);
            if (j.contains("path") && j["path"].is_string())
                path = j["path"].get<std::string>();
        } catch (...) {}
        if (!path.empty())
            push_event({server_event_type::capture_done, path, {}});
    }
}

// ---------------------------------------------------------------------------
// Thread-safe helpers
// ---------------------------------------------------------------------------

void capture_client::push_cmd(cmd c) {
    std::lock_guard lock(cmd_mtx_);
    cmd_queue_.push_back(c);
}

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
