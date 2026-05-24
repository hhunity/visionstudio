#include <httplib.h>
#include <nlohmann/json.hpp>
#include <CLI/CLI.hpp>
#include <turbojpeg.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// SSE broadcaster
// ---------------------------------------------------------------------------

struct sse_sink {
    httplib::DataSink* ptr = nullptr;
    std::mutex         mtx;

    bool send(const std::string& event, const std::string& data) {
        std::lock_guard lock(mtx);
        if (!ptr) return false;
        const std::string msg = "event: " + event + "\ndata: " + data + "\n\n";
        return ptr->write(msg.c_str(), msg.size());
    }

    void set(httplib::DataSink* s) {
        std::lock_guard lock(mtx);
        ptr = s;
    }

    void clear() {
        std::lock_guard lock(mtx);
        ptr = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Capture trigger
// ---------------------------------------------------------------------------

struct capture_trigger {
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    pending  = false;
    bool                    shutdown = false;
};

// ---------------------------------------------------------------------------
// Camera info state
// ---------------------------------------------------------------------------

static std::mutex          cam_info_mtx;
static nlohmann::json      cam_info_state = {
    {"groups", {
        {{"label", "Sensor"}, {"params", {
            {{"name","Model"},      {"type","string"}, {"rw_type","readonly"},  {"value","SL-2048-CL"}},
            {{"name","PixelFormat"},{"type","enum"},   {"rw_type","readwrite"}, {"value","Mono8"},
                {"options", {"Mono8","Mono12","Mono16"}}},
            {{"name","Width"},      {"type","int"},    {"rw_type","readonly"},  {"value","2048"}, {"unit","px"}},
            {{"name","HeightMax"},  {"type","int"},    {"rw_type","readonly"},  {"value","65536"},{"unit","px"}},
        }}},
        {{"label", "Acquisition"}, {"params", {
            {{"name","LineRate"},    {"type","int"},   {"rw_type","readwrite"}, {"value","10000"},
                {"unit","Hz"}, {"min","100"}, {"max","20000"}},
            {{"name","ExposureTime"},{"type","float"}, {"rw_type","readwrite"}, {"value","80.0"},
                {"unit","us"}, {"min","1.0"}, {"max","500.0"}},
            {{"name","TriggerMode"}, {"type","enum"},  {"rw_type","readwrite"}, {"value","Off"},
                {"options", {"Off","External","Software"}}},
            {{"name","ScanMode"},    {"type","enum"},  {"rw_type","readwrite"}, {"value","Continuous"},
                {"options", {"Continuous","SingleFrame"}}},
        }}},
        {{"label", "Transport"}, {"params", {
            {{"name","Interface"},  {"type","string"},{"rw_type","readonly"},  {"value","CameraLink"}},
            {{"name","Bandwidth"},  {"type","int"},   {"rw_type","readonly"},  {"value","680"}, {"unit","MB/s"}},
            {{"name","PacketSize"}, {"type","int"},   {"rw_type","readwrite"}, {"value","8192"},
                {"unit","bytes"}, {"min","512"}, {"max","65536"}},
        }}},
        {{"label", "Status"}, {"params", {
            {{"name","Temperature"},    {"type","float"},{"rw_type","readonly"},  {"value","42.3"}, {"unit","C"}},
            {{"name","FrameCount"},     {"type","int"},  {"rw_type","readonly"},  {"value","0"}},
            {{"name","ErrorCount"},     {"type","int"},  {"rw_type","readonly"},  {"value","0"}},
            {{"name","TriggerSoftware"},{"type","bool"}, {"rw_type","writeonly"}, {"value","false"}},
        }}},
    }}
};

// ---------------------------------------------------------------------------
// Preview helpers
// ---------------------------------------------------------------------------

// Decouples frame generation from encoding/sending.
// If the previous send is still in progress the new frame is silently dropped.
class preview_sender {
    std::future<bool> future_;
public:
    bool try_send(std::function<bool()> fn) {
        if (future_.valid() &&
            future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false; // previous send still in flight — drop this frame
        future_ = std::async(std::launch::async, std::move(fn));
        return true;
    }
    // Wait for any in-flight send to complete (call when the stream loop exits).
    void drain() { if (future_.valid()) future_.get(); }
};

// Grayscale wave pattern that drifts over time.
// phase advances each frame so the pattern scrolls diagonally.
static std::vector<uint8_t> gen_wave_frame(int w, int h, double phase) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double v = 128.0
                + 80.0 * std::sin(2.0 * M_PI * x / 80.0 + phase)
                + 40.0 * std::cos(2.0 * M_PI * y / 24.0 + phase * 0.5)
                + 20.0 * std::sin(2.0 * M_PI * (x + y) / 120.0 + phase * 0.7);
            const int clamped = v < 0.0 ? 0 : (v > 255.0 ? 255 : static_cast<int>(v));
            pixels[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(clamped);
        }
    }
    return pixels;
}

static std::vector<uint8_t> encode_jpeg(const std::vector<uint8_t>& pixels,
                                        int w, int h, int quality = 85) {
    tjhandle tj = tjInitCompress();
    if (!tj) {
        fprintf(stderr, "[mock] tjInitCompress failed\n");
        return {};
    }
    unsigned char* buf      = nullptr;
    unsigned long  buf_size = 0;
    const int rc = tjCompress2(tj,
                               reinterpret_cast<const unsigned char*>(pixels.data()),
                               w, w, h,
                               TJPF_GRAY, &buf, &buf_size, TJSAMP_GRAY, quality, 0);
    if (rc < 0 || !buf || buf_size == 0) {
        fprintf(stderr, "[mock] tjCompress2 failed: %s\n", tjGetErrorStr2(tj));
        if (buf) tjFree(buf);
        tjDestroy(tj);
        return {};
    }
    std::vector<uint8_t> result(buf, buf + buf_size);
    tjFree(buf);
    tjDestroy(tj);
    return result;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    CLI::App app{"Mock camera server for VisionStudio testing"};

    int         port        = 8080;
    std::string tiff_path;
    int         delay_s     = 2;
    int         preview_w   = 512;
    int         preview_h   = 64;
    double      preview_fps = 15.0;

    app.add_option("--port",        port,        "Listen port")->default_val(8080);
    app.add_option("--tiff",        tiff_path,   "Path to TIFF file sent in capture_done event");
    app.add_option("--delay",       delay_s,     "Seconds before capture_done fires")->default_val(2);
    app.add_option("--preview-w",   preview_w,   "Preview frame width in pixels")->default_val(512);
    app.add_option("--preview-h",   preview_h,   "Preview frame height in pixels")->default_val(64);
    app.add_option("--preview-fps", preview_fps, "Preview frame rate")->default_val(15.0);

    CLI11_PARSE(app, argc, argv);

    sse_sink        sink;
    capture_trigger trigger;
    std::atomic<int> frame_count{0};

    // Trigger thread: waits for start_capture, fires capture_done after delay.
    std::thread trigger_thread([&] {
        while (true) {
            std::unique_lock lock(trigger.mtx);
            trigger.cv.wait(lock, [&] { return trigger.pending || trigger.shutdown; });
            if (trigger.shutdown) break;
            trigger.pending = false;
            lock.unlock();

            std::this_thread::sleep_for(std::chrono::seconds(delay_s));

            ++frame_count;

            const nlohmann::json done_data = {
                {"path", tiff_path.empty() ? "" : tiff_path}
            };
            sink.send("capture_done", done_data.dump());
            fprintf(stdout, "[mock] capture_done sent (path=%s)\n",
                    tiff_path.empty() ? "(none)" : tiff_path.c_str());
        }
    });

    httplib::Server svr;

    // GET /events — SSE stream
    svr.Get("/events", [&](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection",    "keep-alive");
        res.set_chunked_content_provider(
            "text/event-stream",
            [&](size_t /*offset*/, httplib::DataSink& ds) {
                sink.set(&ds);
                const std::string msg = "event: connected\ndata: {}\n\n";
                ds.write(msg.c_str(), msg.size());
                fprintf(stdout, "[mock] SSE client connected\n");

                while (ds.is_writable()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                sink.clear();
                fprintf(stdout, "[mock] SSE client disconnected\n");
                return true;
            },
            [&](bool /*success*/) { sink.clear(); }
        );
    });

    // PUT /connect
    svr.Put("/connect", [](const httplib::Request&, httplib::Response& res) {
        fprintf(stdout, "[mock] PUT /connect\n");
        res.set_content("{}", "application/json");
    });

    // POST /start
    svr.Post("/start", [&](const httplib::Request&, httplib::Response& res) {
        fprintf(stdout, "[mock] POST /start\n");
        {
            std::lock_guard lock(trigger.mtx);
            trigger.pending = true;
        }
        trigger.cv.notify_one();
        res.set_content("{}", "application/json");
    });

    // POST /stop
    svr.Post("/stop", [](const httplib::Request&, httplib::Response& res) {
        fprintf(stdout, "[mock] POST /stop\n");
        res.set_content("{}", "application/json");
    });

    // POST /disconnect
    svr.Post("/disconnect", [&](const httplib::Request&, httplib::Response& res) {
        fprintf(stdout, "[mock] POST /disconnect\n");
        sink.send("disconnected", "{}");
        res.set_content("{}", "application/json");
    });

    // GET /info
    svr.Get("/info", [](const httplib::Request&, httplib::Response& res) {
        fprintf(stdout, "[mock] GET /info\n");
        std::lock_guard lock(cam_info_mtx);
        res.set_content(cam_info_state.dump(2), "application/json");
    });

    // PUT /info — update a single parameter by name, return full info
    svr.Put("/info", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto body  = nlohmann::json::parse(req.body);
            const std::string name  = body.value("name",  "");
            const std::string value = body.value("value", "");
            fprintf(stdout, "[mock] PUT /info  name=%s  value=%s\n",
                    name.c_str(), value.c_str());

            std::lock_guard lock(cam_info_mtx);
            for (auto& g : cam_info_state["groups"])
                for (auto& p : g["params"])
                    if (p.value("name", "") == name)
                        p["value"] = value;

            res.set_content(cam_info_state.dump(2), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"bad request\"}", "application/json");
        }
    });

    // GET /preview — MJPEG stream (multipart/x-mixed-replace)
    svr.Get("/preview", [&](const httplib::Request&, httplib::Response& res) {
        fprintf(stdout, "[mock] GET /preview (MJPEG)\n");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection",    "keep-alive");
        const std::string boundary = "frame";
        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=" + boundary,
            [&, boundary](size_t /*offset*/, httplib::DataSink& ds) {
                double phase = 0.0;
                const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double>{1.0 / preview_fps});
                auto next = std::chrono::steady_clock::now();
                preview_sender sender;
                int frame_idx = 0;
                while (ds.is_writable()) {
                    auto pixels = gen_wave_frame(preview_w, preview_h, phase);
                    const bool launched = sender.try_send(
                        [pixels = std::move(pixels), &ds, &boundary,
                         w = preview_w, h = preview_h, idx = frame_idx]() mutable -> bool {
                            auto jpeg = encode_jpeg(pixels, w, h);
                            if (jpeg.empty()) {
                                fprintf(stderr, "[mock] /preview frame %d: encode failed\n", idx);
                                return true;
                            }
                            std::string hdr;
                            hdr += "--" + boundary + "\r\n";
                            hdr += "Content-Type: image/jpeg\r\n";
                            hdr += "Content-Length: " + std::to_string(jpeg.size()) + "\r\n";
                            hdr += "\r\n";
                            if (!ds.write(hdr.c_str(), hdr.size())) return false;
                            if (!ds.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size())) return false;
                            if (!ds.write("\r\n", 2)) return false;
                            fprintf(stdout, "[mock] /preview frame %d sent (%zu bytes)\n",
                                    idx, jpeg.size());
                            return true;
                        });
                    if (!launched)
                        fprintf(stdout, "[mock] /preview frame %d dropped (send busy)\n", frame_idx);
                    ++frame_idx;
                    phase += 0.15;
                    next  += interval;
                    std::this_thread::sleep_until(next);
                }
                sender.drain();
                fprintf(stdout, "[mock] /preview client disconnected\n");
                return true;
            }
        );
    });

    // GET /preview_raw — raw binary stream: [uint32 w][uint32 h][w*h bytes grayscale] per frame
    svr.Get("/preview_raw", [&](const httplib::Request&, httplib::Response& res) {
        fprintf(stdout, "[mock] GET /preview_raw\n");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection",    "keep-alive");
        res.set_chunked_content_provider(
            "application/octet-stream",
            [&](size_t /*offset*/, httplib::DataSink& ds) {
                double phase = 0.0;
                const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double>{1.0 / preview_fps});
                auto next = std::chrono::steady_clock::now();
                preview_sender sender;
                while (ds.is_writable()) {
                    auto pixels = gen_wave_frame(preview_w, preview_h, phase);
                    sender.try_send([pixels = std::move(pixels), &ds,
                                     w = static_cast<uint32_t>(preview_w),
                                     h = static_cast<uint32_t>(preview_h)]() mutable -> bool {
                        if (!ds.write(reinterpret_cast<const char*>(&w), 4)) return false;
                        if (!ds.write(reinterpret_cast<const char*>(&h), 4)) return false;
                        return ds.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
                    });
                    phase += 0.15;
                    next  += interval;
                    std::this_thread::sleep_until(next);
                }
                sender.drain();
                fprintf(stdout, "[mock] /preview_raw client disconnected\n");
                return true;
            }
        );
    });

    fprintf(stdout, "[mock] listening on port %d  (tiff=%s  delay=%ds  preview=%dx%d@%.0ffps)\n",
            port, tiff_path.empty() ? "(none)" : tiff_path.c_str(),
            delay_s, preview_w, preview_h, preview_fps);

    svr.listen("0.0.0.0", port);

    {
        std::lock_guard lock(trigger.mtx);
        trigger.shutdown = true;
    }
    trigger.cv.notify_all();
    trigger_thread.join();

    return 0;
}
