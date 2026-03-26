#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <httplib.h>
#include "capture/capture_config.h"

enum class sse_state { disconnected, connecting, connected, error };

struct evt_connected    {};
struct evt_disconnected {};
struct evt_error        { std::string message; };
struct evt_capture_done { std::string path; };
using server_event = std::variant<evt_connected, evt_disconnected, evt_error, evt_capture_done>;

struct preview_frame {
    std::vector<uint8_t> pixels; // grayscale, w*h bytes
    int w = 0, h = 0;
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

    void start_preview();
    void stop_preview();
    bool poll_preview_frame(preview_frame& out);
    bool is_preview_active() const { return preview_active_.load(); }

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
    void run_preview();
    void run_preview_stream(httplib::Client& cli,
                            const std::string& path,
                            const httplib::Headers& headers,
                            const std::string& log_tag,
                            std::function<void(const httplib::Response&)> on_response,
                            std::function<void(std::vector<uint8_t>&)>    parse_frames);
    void run_preview_mjpeg(httplib::Client& cli);
    void run_preview_raw(httplib::Client& cli);
    void store_preview_frame(int w, int h, std::vector<uint8_t> pixels);
    void log(const std::string& msg) const;

    capture_config cfg_;

    std::thread worker_thread_;
    std::thread sse_thread_;

    httplib::Client   sse_cli_;
    std::atomic<bool> sse_interrupted_{false};

    std::mutex        preview_cli_mtx_;
    httplib::Client*  preview_cli_ptr_{nullptr}; // valid only while run_preview() runs
    std::thread       preview_thread_;
    std::atomic<bool> preview_interrupted_{false};
    std::atomic<bool> preview_active_{false};

    std::mutex     preview_mtx_;
    preview_frame  latest_frame_;
    bool           preview_ready_{false};

    std::mutex              cmd_mtx_;
    std::condition_variable cmd_cv_;
    std::deque<cmd>         cmd_queue_;
    bool                    shutdown_{false};

    std::atomic<sse_state> sse_state_{sse_state::disconnected};

    mutable std::mutex  event_mtx_;
    std::deque<server_event> event_queue_;

    mutable std::mutex logger_mtx_;
    logger_fn          logger_;
};
