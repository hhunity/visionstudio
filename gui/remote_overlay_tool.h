#pragma once
#include "util/analysis_tool.h"
#include "io/overlay_io.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class remote_tool_state { idle, analyzing, done, error };

class remote_overlay_tool : public analysis_tool {
public:
    // Connection settings (editable in UI)
    char host[128]     = "localhost";
    int  port          = 8080;
    char endpoint[128] = "/analyze";

    // Parameter JSON file path (editable in UI)
    char param_path[512] = "";

    std::string_view name() const override { return "Remote Overlay"; }

    // Not pixel-based; image path is set separately via set_image_path().
    void analyze(const image_data&) override {}

    void set_image_path(const std::string& path);

    // Launch HTTP request in background. No-op if already running.
    void run();
    void clear();
    bool is_running() const;

    // Returns true once after a new result arrives. Fills out with a roi_group
    // containing all entries. Caller should pass this to set_overlay_groups().
    bool poll_result(std::vector<roi_group>& out);

    void render_panel() override;
    void render_overlay(ImDrawList*, ImVec2, ImVec2, float, float, float) override {}

private:
    std::string            image_path_;
    std::atomic<remote_tool_state> state_{remote_tool_state::idle};
    std::atomic<bool>      result_ready_{false};
    std::string            error_msg_;
    std::vector<roi_entry> entries_;
    std::mutex             result_mtx_;
    std::thread            worker_;

    void worker_func(std::string host_str, int port_val,
                     std::string endpoint_str,
                     std::string image_path,
                     std::string param_path_str);
};
