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
    interrupt_sse();
    if (worker_thread_.joinable()) worker_thread_.join();
}

// ---------------------------------------------------------------------------
// Public API  (UI thread)
// ---------------------------------------------------------------------------

void capture_client::set_logger(logger_fn fn) {
    std::lock_guard lock(logger_mtx_);
    logger_ = std::move(fn);
}

void capture_client::connect() {
    push_cmd(cmd::connect);
    cmd_cv_.notify_one();
}

void capture_client::disconnect() {
    push_cmd(cmd::disconnect);
    interrupt_sse();
}

void capture_client::start_capture() {
    push_cmd(cmd::start_capture);
    interrupt_sse();
}

void capture_client::stop_capture() {
    push_cmd(cmd::stop_capture);
    interrupt_sse();
}

void capture_client::interrupt_sse() {
    cmd_cv_.notify_one();
    std::lock_guard lock(sse_cli_mtx_);
    if (sse_cli_ptr_) sse_cli_ptr_->stop();
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void capture_client::worker_thread_func() {
    while (true) {
        {
            std::unique_lock lock(cmd_mtx_);
            cmd_cv_.wait(lock, [&] { return !cmd_queue_.empty() || shutdown_; });
            if (shutdown_) return;
            const cmd c = cmd_queue_.front();
            cmd_queue_.pop_front();
            if (c != cmd::connect) continue;
        }

        bool disconnect_requested = false;
        while (!disconnect_requested && !shutdown_) {
            run_sse();
            if (shutdown_) break;

            std::unique_lock lock(cmd_mtx_);
            while (!cmd_queue_.empty()) {
                const cmd c = cmd_queue_.front();
                cmd_queue_.pop_front();
                lock.unlock();
                switch (c) {
                case cmd::start_capture:
                    if (!do_simple_post(cfg_.start_path, "start"))
                        push_event({server_event_type::error, {}, get_last_error()});
                    break;
                case cmd::stop_capture:
                    if (!do_simple_post(cfg_.stop_path, "stop"))
                        push_event({server_event_type::error, {}, get_last_error()});
                    break;
                case cmd::disconnect:
                    disconnect_requested = true;
                    break;
                default:
                    break;
                }
                lock.lock();
                if (disconnect_requested) break;
            }
        }

        if (disconnect_requested)
            do_simple_post(cfg_.disconnect_path, "disconnect");
    }
}

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------

void capture_client::run_sse() {
    httplib::Client sse_cli(cfg_.host, cfg_.port);
    sse_cli.set_read_timeout(0, 0);

    {
        std::lock_guard lock(sse_cli_mtx_);
        sse_cli_ptr_ = &sse_cli;
    }

    const std::string url = cfg_.host + ":" + std::to_string(cfg_.port) + cfg_.sse_path;
    log("[sse] GET " + url);

    std::string buf, cur_event, cur_data;

    sse_cli.Get(
        cfg_.sse_path,
        httplib::Headers{{"Accept",        "text/event-stream"},
                         {"Cache-Control", "no-cache"},
                         {"Connection",    "keep-alive"}},

        [&](const httplib::Response& r) -> bool {
            log("[sse] GET " + url + " -> " + std::to_string(r.status));
            if (r.status < 200 || r.status >= 300) {
                const std::string msg = "SSE: HTTP " + std::to_string(r.status);
                set_error(msg);
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
            log("[sse] raw << " + std::string(data, len));
            buf.append(data, len);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                if (line.empty()) {
                    if (!cur_event.empty() || !cur_data.empty()) {
                        log("[sse] event=" + cur_event + " data=" + cur_data);
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
            return !shutdown_;
        });

    {
        std::lock_guard lock(sse_cli_mtx_);
        sse_cli_ptr_ = nullptr;
    }

    log("[sse] GET " + url + " closed");
    if (sse_state_.load() == sse_state::connected)
        sse_state_.store(sse_state::disconnected);
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

httplib::Client capture_client::make_cli() const {
    httplib::Client cli(cfg_.host, cfg_.port);
    const int sec  = cfg_.timeout_ms / 1000;
    const int usec = (cfg_.timeout_ms % 1000) * 1000;
    cli.set_connection_timeout(sec, usec);
    cli.set_read_timeout(sec, usec);
    return cli;
}

bool capture_client::do_connect_post() {
    auto cli = make_cli();

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

    const std::string boundary = "----VisionStudioBoundary";
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

    const std::string url = cfg_.host + ":" + std::to_string(cfg_.port) + cfg_.connect_path;
    log("[http] PUT " + url);
    const std::string ct = "multipart/form-data; boundary=" + boundary;
    auto res = cli.Put(cfg_.connect_path, body, ct.c_str());
    if (!res) {
        const std::string msg = "connect: PUT failed";
        log("[http] PUT " + url + " -> (no response)");
        set_error(msg);
        return false;
    }
    log("[http] PUT " + url + " -> " + std::to_string(res->status) +
        (res->body.empty() ? "" : "  body=" + res->body));
    if (res->status < 200 || res->status >= 300) {
        set_error("connect: HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

bool capture_client::do_simple_post(const std::string& path,
                                    const std::string& label) {
    const std::string url = cfg_.host + ":" + std::to_string(cfg_.port) + path;
    log("[http] POST " + url);
    auto cli = make_cli();
    auto res = cli.Post(path);
    if (!res) {
        log("[http] POST " + url + " -> (no response)");
        set_error(label + ": POST failed");
        return false;
    }
    log("[http] POST " + url + " -> " + std::to_string(res->status) +
        (res->body.empty() ? "" : "  body=" + res->body));
    if (res->status < 200 || res->status >= 300) {
        set_error(label + ": HTTP " + std::to_string(res->status));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SSE event dispatch
// ---------------------------------------------------------------------------

void capture_client::dispatch_event(const std::string& event_type,
                                    const std::string& data) {
    auto get_field = [&](const char* key) -> std::string {
        try {
            auto j = nlohmann::json::parse(data);
            if (j.contains(key) && j[key].is_string())
                return j[key].get<std::string>();
        } catch (...) {}
        return {};
    };

    if (event_type == "connected") {
        push_event({server_event_type::connected, {}, {}});
    } else if (event_type == "disconnected") {
        push_event({server_event_type::disconnected, {}, {}});
    } else if (event_type == "error") {
        push_event({server_event_type::error, {}, get_field("message")});
    } else if (event_type == "capture_done") {
        const auto path = get_field("path");
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
    event_queue_.pop_front();
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

void capture_client::log(const std::string& msg) const {
    std::lock_guard lock(logger_mtx_);
    if (logger_) logger_(msg);
}
