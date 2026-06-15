#include "db/overlay_helper.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <string>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string path_stem(const std::string& path) {
    const auto slash = path.rfind('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

// ---------------------------------------------------------------------------
// convert_overlay_jsonl_to_json
// ---------------------------------------------------------------------------

bool convert_overlay_jsonl_to_json(const std::string& src, const std::string& dst) {
    std::ifstream in(src);
    if (!in) return false;

    struct tile_key {
        int w, h;
        bool operator<(const tile_key& o) const {
            return w != o.w ? w < o.w : h < o.h;
        }
    };

    struct entry_data { int col, row; float dx, dy, angle; };
    std::map<tile_key, std::vector<entry_data>> groups;
    const std::string stem = path_stem(src);

    std::string line;
    try {
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            const auto j = nlohmann::json::parse(line);
            const auto& roi  = j.at("roi");
            const auto& info = j.at("info");
            const int rx = roi.at("x").get<int>();
            const int ry = roi.at("y").get<int>();
            const int rw = roi.at("w").get<int>();
            const int rh = roi.at("h").get<int>();
            if (rw <= 0 || rh <= 0) continue;
            entry_data e;
            e.col   = rx / rw;
            e.row   = ry / rh;
            e.dx    = info.value("dx",    0.0f);
            e.dy    = info.value("dy",    0.0f);
            e.angle = info.value("angle", 0.0f);
            groups[{rw, rh}].push_back(e);
        }
    } catch (...) {
        return false;
    }

    nlohmann::json out;
    out["version"] = 1;
    out["groups"]  = nlohmann::json::array();
    for (const auto& [k, entries] : groups) {
        nlohmann::json jg;
        jg["label"]   = stem;
        jg["size"]    = {{"w", k.w}, {"h", k.h}};
        jg["entries"] = nlohmann::json::array();
        for (const auto& e : entries) {
            jg["entries"].push_back({
                {"x", e.col}, {"y", e.row},
                {"dx", e.dx}, {"dy", e.dy}, {"angle", e.angle}
            });
        }
        out["groups"].push_back(std::move(jg));
    }

    std::ofstream f(dst);
    if (!f) return false;
    f << out.dump(2) << '\n';
    return f.good();
}
