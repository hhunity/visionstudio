#pragma once
#include <vector>
#include <imgui.h>
#include <implot.h>
#include <nlohmann/json.hpp>
#include "gui/viewer_context.h"

class overlay_graph_panel {
public:
    bool   visible    = false;
    bool   show_dx    = true;
    bool   show_dy    = true;
    bool   show_angle = true;
    bool   show_fit   = true;
    bool   show_ref   = false;
    double ref_a      = 0.0;
    double ref_b      = 0.0;

    // Call once per frame inside the ImGui render loop.
    // single_viewer / compare are non-const because overlay_group_visibility is mutable.
    void render(const viewer_context& ctx,
                image_viewer& single_viewer,
                compare_viewer& compare);

    void load_settings(const nlohmann::json& j);
    nlohmann::json save_settings() const;
};
