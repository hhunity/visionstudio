#include "io/overlay_io.h"
#include <nlohmann/json.hpp>
#include <fstream>

bool overlay_io::load(const std::string& path, std::vector<roi_group>& out) {
    std::ifstream f(path);
    if (!f) return false;

    try {
        const auto j = nlohmann::json::parse(f);
        out.clear();
        for (const auto& jg : j.at("groups")) {
            roi_group g;
            g.label  = jg.value("label", "");
            g.tile_w = jg.at("size").at("w").get<int>();
            g.tile_h = jg.at("size").at("h").get<int>();
            for (const auto& je : jg.at("entries")) {
                roi_entry e;
                const int col = je.at("x").get<int>();
                const int row = je.at("y").get<int>();
                e.x     = col * g.tile_w;
                e.y     = row * g.tile_h;
                e.w     = g.tile_w;
                e.h     = g.tile_h;
                e.dx    = je.value("dx",    0.0f);
                e.dy    = je.value("dy",    0.0f);
                e.angle = je.value("angle", 0.0f);
                e.label = g.label;
                g.entries.push_back(e);
            }
            out.push_back(std::move(g));
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool overlay_io::load_flat(const std::string& path, std::vector<roi_entry>& out) {
    std::vector<roi_group> groups;
    if (!load(path, groups)) return false;
    out.clear();
    for (const auto& g : groups)
        for (const auto& e : g.entries)
            out.push_back(e);
    return true;
}
