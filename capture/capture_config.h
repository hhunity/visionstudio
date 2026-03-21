#pragma once
#include <string>
#include <vector>

struct capture_config {
    std::string              host            = "localhost";
    int                      port            = 8080;
    std::string              connect_path    = "/connect";
    std::string              start_path      = "/start";
    std::string              stop_path       = "/stop";
    std::string              disconnect_path = "/disconnect";
    std::string              sse_path        = "/events";
    int                      timeout_ms      = 5000;
    std::vector<std::string> config_files; // paths to camera JSON config files

    // Load capture settings from visionstudio.json.
    // Returns defaults if the file is not found or the "capture" key is missing.
    static capture_config load(const std::string& json_path = "visionstudio.json");

    // Save capture settings + imgui_ini into visionstudio.json.
    // If the file already exists, other keys are preserved.
    static void save(const std::string& json_path,
                     const capture_config& cfg,
                     const std::string& imgui_ini = {});

    // Read the "imgui_ini" string from visionstudio.json.
    // Returns empty string if not present.
    static std::string load_imgui_ini(const std::string& json_path = "visionstudio.json");
};
