#include "capture/capture_config.h"
#include <nlohmann/json.hpp>
#include <fstream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static nlohmann::json load_json(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return nlohmann::json::object();
    try {
        return nlohmann::json::parse(f);
    } catch (...) {
        return nlohmann::json::object();
    }
}

// ---------------------------------------------------------------------------
// capture_config
// ---------------------------------------------------------------------------

capture_config capture_config::load(const std::string& json_path) {
    capture_config cfg;
    const auto root = load_json(json_path);
    if (!root.contains("capture")) return cfg;
    const auto& c = root["capture"];

    if (c.contains("host")            && c["host"].is_string())           cfg.host            = c["host"];
    if (c.contains("connect_path")    && c["connect_path"].is_string())    cfg.connect_path    = c["connect_path"];
    if (c.contains("start_path")      && c["start_path"].is_string())      cfg.start_path      = c["start_path"];
    if (c.contains("stop_path")       && c["stop_path"].is_string())       cfg.stop_path       = c["stop_path"];
    if (c.contains("disconnect_path") && c["disconnect_path"].is_string()) cfg.disconnect_path = c["disconnect_path"];
    if (c.contains("sse_path")        && c["sse_path"].is_string())        cfg.sse_path        = c["sse_path"];
    if (c.contains("port")            && c["port"].is_number_integer())    cfg.port            = c["port"];
    if (c.contains("timeout_ms")      && c["timeout_ms"].is_number_integer()) cfg.timeout_ms   = c["timeout_ms"];
    if (c.contains("config_files") && c["config_files"].is_array()) {
        for (const auto& f : c["config_files"])
            if (f.is_string()) cfg.config_files.push_back(f.get<std::string>());
    }
    return cfg;
}

void capture_config::save(const std::string& json_path,
                           const capture_config& cfg,
                           const std::string& imgui_ini) {
    // Preserve existing keys, overwrite only ours.
    auto root = load_json(json_path);

    root["capture"] = {
        {"host",            cfg.host},
        {"port",            cfg.port},
        {"connect_path",    cfg.connect_path},
        {"start_path",      cfg.start_path},
        {"stop_path",       cfg.stop_path},
        {"disconnect_path", cfg.disconnect_path},
        {"sse_path",        cfg.sse_path},
        {"timeout_ms",      cfg.timeout_ms},
        {"config_files",    cfg.config_files},
    };

    if (!imgui_ini.empty())
        root["imgui_ini"] = imgui_ini;

    std::ofstream f(json_path);
    if (f.is_open())
        f << root.dump(2) << '\n';
}

std::string capture_config::load_imgui_ini(const std::string& json_path) {
    const auto root = load_json(json_path);
    if (root.contains("imgui_ini") && root["imgui_ini"].is_string())
        return root["imgui_ini"].get<std::string>();
    return {};
}
