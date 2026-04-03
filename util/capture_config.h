#pragma once
#include <string>

struct capture_config {
    // HTTP client settings (stored under "capture_client" in visionstudio.json)
    std::string host            = "localhost";
    int         port            = 8080;
    std::string connect_path    = "/connect";
    std::string start_path      = "/start";
    std::string stop_path       = "/stop";
    std::string disconnect_path = "/disconnect";
    std::string sse_path        = "/events";
    std::string preview_path    = "/preview";
    std::string preview_raw_path = "/preview_raw";
    bool        preview_raw     = false;  // true = raw pixel stream, false = MJPEG
    int         timeout_ms      = 5000;

    // App-level capture settings (stored under "capture" in visionstudio.json)
    std::string connect_config_file; // path to connection config file
    std::string capture_config_file; // path to capture config file (JSON)
    std::string save_dir;            // local directory to save downloaded captures (empty = default)

    bool operator==(const capture_config&) const = default;
    bool operator!=(const capture_config& o) const { return !(*this == o); }

    // Load settings from visionstudio.json.
    // Network settings come from "capture_client"; config file paths from "capture".
    // Returns defaults if the file is not found or keys are missing.
    static capture_config load(const std::string& json_path = "visionstudio.json");

    // Save settings into visionstudio.json.
    // Network settings saved under "capture_client"; config file paths under "capture".
    // If the file already exists, other keys are preserved.
    static void save(const std::string& json_path,
                     const capture_config& cfg,
                     const std::string& imgui_ini = {});

    // Read the "imgui_ini" string from visionstudio.json.
    // Returns empty string if not present.
    static std::string load_imgui_ini(const std::string& json_path = "visionstudio.json");
};
