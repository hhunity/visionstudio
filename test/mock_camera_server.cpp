#include <httplib.h>
#include <nlohmann/json.hpp>
#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// SSE broadcaster — holds one active sink (the connected client).
// The camera server only supports a single SSE client at a time.
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
// Capture trigger — start_capture posts here, the trigger thread fires
// capture_done after the configured delay.
// ---------------------------------------------------------------------------

struct capture_trigger {
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    pending  = false;
    bool                    shutdown = false;
};

// ---------------------------------------------------------------------------
// Camera info JSON (GET /info)
// ---------------------------------------------------------------------------

static nlohmann::json make_camera_info() {
    return {
        {"groups", {
            {{"label", "Sensor"}, {"params", {
                {{"name","Model"},      {"type","string"}, {"rw_type","readonly"},  {"value","SL-2048-CL"}},
                {{"name","PixelFormat"},{"type","enum"},   {"rw_type","readwrite"}, {"value","Mono8"},
                    {"options", {"Mono8","Mono12","Mono16"}}},
                {{"name","Width"},      {"type","int"},    {"rw_type","readonly"},  {"value","2048"}, {"unit","px"}},
                {{"name","HeightMax"}, {"type","int"},    {"rw_type","readonly"},  {"value","65536"},{"unit","px"}},
            }}},
            {{"label", "Acquisition"}, {"params", {
                {{"name","LineRate"},   {"type","int"},   {"rw_type","readwrite"}, {"value","10000"},
                    {"unit","Hz"}, {"min","100"}, {"max","20000"}},
                {{"name","ExposureTime"},{"type","float"},{"rw_type","readwrite"}, {"value","80.0"},
                    {"unit","us"}, {"min","1.0"}, {"max","500.0"}},
                {{"name","TriggerMode"},{"type","enum"},  {"rw_type","readwrite"}, {"value","Off"},
                    {"options", {"Off","External","Software"}}},
                {{"name","ScanMode"},   {"type","enum"},  {"rw_type","readwrite"}, {"value","Continuous"},
                    {"options", {"Continuous","SingleFrame"}}},
            }}},
            {{"label", "Transport"}, {"params", {
                {{"name","Interface"},  {"type","string"},{"rw_type","readonly"},  {"value","CameraLink"}},
                {{"name","Bandwidth"},  {"type","int"},   {"rw_type","readonly"},  {"value","680"}, {"unit","MB/s"}},
                {{"name","PacketSize"}, {"type","int"},   {"rw_type","readwrite"}, {"value","8192"},
                    {"unit","bytes"}, {"min","512"}, {"max","65536"}},
            }}},
            {{"label", "Status"}, {"params", {
                {{"name","Temperature"},{"type","float"},{"rw_type","readonly"},  {"value","42.3"}, {"unit","C"}},
                {{"name","FrameCount"}, {"type","int"},  {"rw_type","readonly"},  {"value","0"}},
                {{"name","ErrorCount"}, {"type","int"},  {"rw_type","readonly"},  {"value","0"}},
                {{"name","TriggerSoftware"},{"type","bool"},{"rw_type","writeonly"},{"value","false"}},
            }}},
        }}
    };
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    CLI::App app{"Mock camera server for VisionStudio testing"};

    int         port       = 8080;
    std::string tiff_path;
    int         delay_s    = 2;

    app.add_option("--port",  port,      "Listen port")->default_val(8080);
    app.add_option("--tiff",  tiff_path, "Path to TIFF file sent in capture_done event");
    app.add_option("--delay", delay_s,   "Seconds before capture_done fires")->default_val(2);

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

            // Patch frame count in the status group (just for show).
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
                // Send connected event immediately so the client can proceed with PUT /connect.
                const std::string msg = "event: connected\ndata: {}\n\n";
                ds.write(msg.c_str(), msg.size());
                fprintf(stdout, "[mock] SSE client connected\n");

                // Block until the sink is invalidated (client disconnects or server stops).
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
        res.set_content(make_camera_info().dump(2), "application/json");
    });

    fprintf(stdout, "[mock] listening on port %d  (tiff=%s  delay=%ds)\n",
            port, tiff_path.empty() ? "(none)" : tiff_path.c_str(), delay_s);

    svr.listen("0.0.0.0", port);

    // Shutdown trigger thread.
    {
        std::lock_guard lock(trigger.mtx);
        trigger.shutdown = true;
    }
    trigger.cv.notify_all();
    trigger_thread.join();

    return 0;
}
