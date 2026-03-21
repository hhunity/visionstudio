#include "io/jsonl_io.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

bool jsonl_io::load(const std::string& path, std::vector<roi_entry>& out) {
    std::ifstream f(path);
    if (!f) return false;

    out.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        try {
            const auto j = nlohmann::json::parse(line);
            roi_entry e;

            const auto& roi = j.at("roi");
            e.x = roi.at("x").get<int>();
            e.y = roi.at("y").get<int>();
            e.w = roi.at("w").get<int>();
            e.h = roi.at("h").get<int>();

            if (j.contains("info")) {
                const auto& info = j.at("info");
                e.dx    = info.value("dx",    0.0f);
                e.dy    = info.value("dy",    0.0f);
                e.angle = info.value("angle", 0.0f);
            }
            e.label = j.value("label", "");
            out.push_back(e);
        } catch (...) {
            // skip malformed lines
        }
    }
    return true;
}
