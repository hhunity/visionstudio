#include "capture/capture_client.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <turbojpeg.h>
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

capture_client::capture_client(capture_config cfg)
    : cfg_(std::move(cfg))
{
    worker_thread_ = std::thread(&capture_client::worker_thread_func, this);
}

capture_client::~capture_client() {
    {
        std::lock_guard lock(cmd_mtx_);
        shutdown_ = true;
    }
    cmd_cv_.notify_all();
    if (worker_thread_.joinable())  worker_thread_.join();
    interrupt_sse();
    stop_preview();
    cancel_download();
    if (sse_thread_.joinable())     sse_thread_.join();
    if (preview_thread_.joinable()) preview_thread_.join();
    if (dl_thread_.joinable())      dl_thread_.join();
    if (ul_thread_.joinable())      ul_thread_.join();
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
    cmd_cv_.notify_one();
}

void capture_client::stop_capture() {
    push_cmd(cmd::stop_capture);
    cmd_cv_.notify_one();
}

void capture_client::interrupt_sse() {
    sse_interrupted_ = true;
    std::lock_guard lock(sse_cli_mtx_);
    if (sse_cli_ptr_) sse_cli_ptr_->stop();
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void capture_client::worker_thread_func() {
    while (true) {
        std::unique_lock lock(cmd_mtx_);
        cmd_cv_.wait(lock, [&] { return !cmd_queue_.empty() || shutdown_; });
        if (shutdown_) return;

        const cmd c = cmd_queue_.front();
        cmd_queue_.pop_front();
        lock.unlock();

        const sse_state state = sse_state_.load();

        switch (c) {
        case cmd::connect:
            if (state != sse_state::disconnected && state != sse_state::error) break;
            sse_state_.store(sse_state::connecting);
            sse_interrupted_ = false;
            if (sse_thread_.joinable()) sse_thread_.join();
            sse_thread_ = std::thread([this] { run_sse(); });
            break;

        case cmd::start_capture:
            if (state != sse_state::connected) break;
            do_simple_post(cfg_.start_path, "start");
            break;

        case cmd::stop_capture:
            if (state != sse_state::connected) break;
            do_simple_post(cfg_.stop_path, "stop");
            break;

        case cmd::disconnect:
            if (state == sse_state::disconnected) break;
            interrupt_sse();
            // Do not join here; httplib stop() may take time to unblock Get().
            // sse_thread_ is joined on the next connect() or in the destructor.
            do_simple_post(cfg_.disconnect_path, "disconnect");
            sse_state_.store(sse_state::disconnected);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------

void capture_client::run_sse() {
    httplib::Client cli(cfg_.host, cfg_.port);
    {
        const int sec  = cfg_.timeout_ms / 1000;
        const int usec = (cfg_.timeout_ms % 1000) * 1000;
        cli.set_connection_timeout(sec, usec);
        cli.set_read_timeout(3600, 0);
    }
    {
        std::lock_guard lock(sse_cli_mtx_);
        sse_cli_ptr_ = &cli;
    }

    const std::string url = cfg_.host + ":" + std::to_string(cfg_.port) + cfg_.sse_path;
    log("[sse] GET " + url);

    std::string buf, cur_event, cur_data;

    cli.Get(
        cfg_.sse_path,
        httplib::Headers{{"Accept",        "text/event-stream"},
                         {"Cache-Control", "no-cache"},
                         {"Connection",    "keep-alive"}},

        [&](const httplib::Response& r) -> bool {
            log("[sse] GET " + url + " -> " + std::to_string(r.status));
            if (r.status < 200 || r.status >= 300) {
                push_event(evt_error{"SSE: HTTP " + std::to_string(r.status)});
                sse_state_.store(sse_state::error);
                return false;
            }
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
            return !shutdown_ && !sse_interrupted_;
        });

    {
        std::lock_guard lock(sse_cli_mtx_);
        sse_cli_ptr_ = nullptr;
    }
    log("[sse] GET " + url + " closed");
    const auto s = sse_state_.load();
    if (s != sse_state::error && s != sse_state::disconnected) {
        sse_state_.store(sse_state::disconnected);
        push_event(evt_disconnected{});
    }
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

    if (!cfg_.connect_config_file.empty()) {
        const auto& file_path = cfg_.connect_config_file;
        std::ifstream f(file_path, std::ios::binary);
        if (!f.is_open()) {
            push_event(evt_error{"connect: cannot open config file: " + file_path});
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
        log("[http] PUT " + url + " -> (no response)");
        push_event(evt_error{"connect: PUT failed"});
        return false;
    }
    log("[http] PUT " + url + " -> " + std::to_string(res->status) +
        (res->body.empty() ? "" : "  body=" + res->body));
    if (res->status < 200 || res->status >= 300) {
        push_event(evt_error{"connect: HTTP " + std::to_string(res->status)});
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
        push_event(evt_error{label + ": POST failed"});
        return false;
    }
    log("[http] POST " + url + " -> " + std::to_string(res->status) +
        (res->body.empty() ? "" : "  body=" + res->body));
    if (res->status < 200 || res->status >= 300) {
        push_event(evt_error{label + ": HTTP " + std::to_string(res->status)});
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
        if (!do_connect_post()) {
            sse_state_.store(sse_state::error);
            push_event(evt_error{"connect POST failed"});
        } else {
            sse_state_.store(sse_state::connected);
            push_event(evt_connected{});
        }
    } else if (event_type == "disconnected") {
        sse_state_.store(sse_state::disconnected);
        push_event(evt_disconnected{});
    } else if (event_type == "error") {
        push_event(evt_error{get_field("message")});
    } else if (event_type == "capture_done") {
        const auto path = get_field("path");
        if (!path.empty())
            push_event(evt_capture_done{path});
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

// ---------------------------------------------------------------------------
// MJPEG preview
// ---------------------------------------------------------------------------

void capture_client::start_preview() {
    if (preview_active_.load()) return;
    preview_interrupted_ = false;
    preview_active_      = true;
    if (preview_thread_.joinable()) preview_thread_.join();
    preview_thread_ = std::thread([this] { run_preview(); });
}

void capture_client::stop_preview() {
    if (!preview_active_.load()) return;
    preview_interrupted_ = true;
    std::lock_guard lock(preview_cli_mtx_);
    if (preview_cli_ptr_) preview_cli_ptr_->stop();
    // join happens in the next start_preview() or destructor
}

bool capture_client::poll_preview_frame(preview_frame& out) {
    std::lock_guard lock(preview_mtx_);
    if (!preview_ready_) return false;
    out           = std::move(latest_frame_);
    preview_ready_ = false;
    return true;
}

void capture_client::run_preview() {
    httplib::Client cli(cfg_.host, cfg_.port);
    cli.set_read_timeout(3600, 0);
    {
        std::lock_guard lock(preview_cli_mtx_);
        preview_cli_ptr_ = &cli;
    }

    if (cfg_.preview_raw)
        run_preview_raw(cli);
    else
        run_preview_mjpeg(cli);

    {
        std::lock_guard lock(preview_cli_mtx_);
        preview_cli_ptr_ = nullptr;
    }
    preview_active_ = false;
}

// ---------------------------------------------------------------------------
// MJPEG / raw preview
// ---------------------------------------------------------------------------

void capture_client::store_preview_frame(int w, int h, std::vector<uint8_t> pixels) {
    std::lock_guard lock(preview_mtx_);
    latest_frame_ = {std::move(pixels), w, h};
    preview_ready_ = true;
}

void capture_client::run_preview_mjpeg(httplib::Client& cli) {
    const std::string url = cfg_.host + ":" + std::to_string(cfg_.port) + cfg_.preview_path;
    log("[preview] GET " + url);

    tjhandle tj = tjInitDecompress();
    if (!tj) { push_event(evt_error{"preview: tjInitDecompress failed"}); return; }

    std::string          boundary;
    std::vector<uint8_t> buf;

    cli.Get(
        cfg_.preview_path,
        httplib::Headers{{"Accept", "multipart/x-mixed-replace, image/jpeg"}},

        [&](const httplib::Response& r) -> bool {
            log("[preview] GET " + url + " -> " + std::to_string(r.status));
            if (r.status < 200 || r.status >= 300) {
                push_event(evt_error{"preview: HTTP " + std::to_string(r.status)});
                return false;
            }
            const auto ct  = r.get_header_value("Content-Type");
            const auto pos = ct.find("boundary=");
            if (pos != std::string::npos) {
                boundary = ct.substr(pos + 9);
                if (!boundary.empty() && boundary.front() == '"') boundary = boundary.substr(1);
                if (!boundary.empty() && boundary.back()  == '"') boundary.pop_back();
            }
            return true;
        },

        [&](const char* data, size_t len) -> bool {
            buf.insert(buf.end(), data, data + len);

            const std::string bnd     = "--" + boundary + "\r\n";
            const std::string hdr_end = "\r\n\r\n";

            while (true) {
                auto it = std::search(buf.begin(), buf.end(), bnd.begin(), bnd.end());
                if (it == buf.end()) break;
                auto after_bnd = it + static_cast<std::ptrdiff_t>(bnd.size());
                auto hdr_it    = std::search(after_bnd, buf.end(),
                                             hdr_end.begin(), hdr_end.end());
                if (hdr_it == buf.end()) break;

                const std::string part_headers(after_bnd, hdr_it);
                int content_length = 0;
                for (const char* key : {"Content-Length:", "content-length:"}) {
                    const auto cl = part_headers.find(key);
                    if (cl != std::string::npos) {
                        content_length = std::stoi(part_headers.substr(cl + std::strlen(key)));
                        break;
                    }
                }
                if (content_length <= 0) {
                    buf.erase(buf.begin(), hdr_it + static_cast<std::ptrdiff_t>(hdr_end.size()));
                    continue;
                }
                auto data_start = hdr_it + static_cast<std::ptrdiff_t>(hdr_end.size());
                if (static_cast<int>(buf.end() - data_start) < content_length) break;

                int w = 0, h = 0, jpeg_sub = 0, jpeg_cs = 0;
                if (tjDecompressHeader3(tj,
                        reinterpret_cast<const unsigned char*>(&*data_start),
                        static_cast<unsigned long>(content_length),
                        &w, &h, &jpeg_sub, &jpeg_cs) == 0 && w > 0 && h > 0) {
                    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h);
                    if (tjDecompress2(tj,
                            reinterpret_cast<const unsigned char*>(&*data_start),
                            static_cast<unsigned long>(content_length),
                            pixels.data(), w, w, h,
                            TJPF_GRAY, TJFLAG_FASTDCT) == 0)
                        store_preview_frame(w, h, std::move(pixels));
                }
                buf.erase(buf.begin(), data_start + content_length);
            }
            return !preview_interrupted_;
        });

    tjDestroy(tj);
    log("[preview] GET " + url + " closed");
}

void capture_client::run_preview_raw(httplib::Client& cli) {
    const std::string url = cfg_.host + ":" + std::to_string(cfg_.port) + cfg_.preview_raw_path;
    log("[preview_raw] GET " + url);

    std::vector<uint8_t> buf;

    cli.Get(
        cfg_.preview_raw_path,
        httplib::Headers{{"Accept", "application/octet-stream"}},

        [&](const httplib::Response& r) -> bool {
            log("[preview_raw] GET " + url + " -> " + std::to_string(r.status));
            if (r.status < 200 || r.status >= 300) {
                push_event(evt_error{"preview_raw: HTTP " + std::to_string(r.status)});
                return false;
            }
            return true;
        },

        [&](const char* data, size_t len) -> bool {
            buf.insert(buf.end(), data, data + len);
            // Frame format: [uint32 width][uint32 height][w*h bytes gray]
            while (buf.size() >= 8) {
                uint32_t w = 0, h = 0;
                std::memcpy(&w, buf.data(),     4);
                std::memcpy(&h, buf.data() + 4, 4);
                const size_t frame_size = 8 + static_cast<size_t>(w) * h;
                if (buf.size() < frame_size) break;
                std::vector<uint8_t> pixels(buf.begin() + 8,
                                            buf.begin() + static_cast<ptrdiff_t>(frame_size));
                store_preview_frame(static_cast<int>(w), static_cast<int>(h), std::move(pixels));
                buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(frame_size));
            }
            return !preview_interrupted_;
        });

    log("[preview_raw] GET " + url + " closed");
}

void capture_client::log(const std::string& msg) const {
    std::lock_guard lock(logger_mtx_);
    if (logger_) logger_(msg);
}

// ---------------------------------------------------------------------------
// File download
// ---------------------------------------------------------------------------

void capture_client::start_download(const std::string& url_path,
                                    const std::string& dest_path) {
    if (dl_thread_.joinable()) dl_thread_.join();
    download_active_   = true;
    download_progress_ = 0.0f;
    dl_thread_ = std::thread([this, url_path, dest_path] {
        run_download(url_path, dest_path);
    });
}

void capture_client::run_download(std::string url_path, std::string dest_path) {
    httplib::Client cli(cfg_.host, cfg_.port);
    {
        const int sec  = cfg_.timeout_ms / 1000;
        const int usec = (cfg_.timeout_ms % 1000) * 1000;
        cli.set_connection_timeout(sec, usec);
        cli.set_read_timeout(3600, 0);
    }

    log("[download] GET " + url_path + " -> " + dest_path);

    auto res = cli.Get(url_path);

    if (!res || res->status < 200 || res->status >= 300) {
        log("[download] failed");
        download_progress_ = -1.0f;
    } else {
        std::ofstream ofs(dest_path, std::ios::binary);
        ofs.write(res->body.data(), static_cast<std::streamsize>(res->body.size()));
        log("[download] done, " + std::to_string(res->body.size()) + " bytes");
        download_progress_ = 1.0f;
    }
    download_active_ = false;
}

// ---------------------------------------------------------------------------
// File upload
// ---------------------------------------------------------------------------

void capture_client::start_upload(const std::string& url_path,
                                  const std::string& src_path,
                                  const std::string& field_name,
                                  const std::string& content_type) {
    if (ul_thread_.joinable()) ul_thread_.join();
    upload_active_   = true;
    upload_progress_ = 0.0f;
    ul_thread_ = std::thread([this, url_path, src_path, field_name, content_type] {
        run_upload(url_path, src_path, field_name, content_type);
    });
}

void capture_client::run_upload(std::string url_path, std::string src_path,
                                std::string field_name, std::string content_type) {
    std::ifstream ifs(src_path, std::ios::binary);
    if (!ifs) {
        log("[upload] cannot open src: " + src_path);
        upload_progress_ = -1.0f;
        upload_active_   = false;
        return;
    }
    const std::string body(std::istreambuf_iterator<char>(ifs), {});

    // filename component of src_path
    const std::string filename = src_path.substr(src_path.find_last_of("/\\") + 1);

    httplib::Client cli(cfg_.host, cfg_.port);
    {
        const int sec  = cfg_.timeout_ms / 1000;
        const int usec = (cfg_.timeout_ms % 1000) * 1000;
        cli.set_connection_timeout(sec, usec);
        cli.set_write_timeout(3600, 0);
        cli.set_read_timeout(3600, 0);
    }

    log("[upload] PUT " + url_path + " <- " + src_path
        + " (" + std::to_string(body.size()) + " bytes)");

    const std::string boundary = "----VisionStudioBoundary";
    std::string mp;
    mp += "--" + boundary + "\r\n";
    mp += "Content-Disposition: form-data; name=\"" + field_name
        + "\"; filename=\"" + filename + "\"\r\n";
    mp += "Content-Type: " + content_type + "\r\n";
    mp += "\r\n";
    mp += body;
    mp += "\r\n--" + boundary + "--\r\n";

    auto res = cli.Put(url_path, mp, "multipart/form-data; boundary=" + boundary);

    if (!res || res->status < 200 || res->status >= 300) {
        log("[upload] failed");
        upload_progress_ = -1.0f;
    } else {
        log("[upload] done, " + std::to_string(body.size()) + " bytes");
        upload_progress_ = 1.0f;
    }
    upload_active_ = false;
}
