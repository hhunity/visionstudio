#pragma once
#include <vector>
#include <imgui.h>
#include <implot.h>
#include <nlohmann/json.hpp>
#include "gui/panel_base.h"

class overlay_graph_panel : public panel_base {
public:
    // visible is inherited from panel_base
    bool   show_dx    = true;
    bool   show_dy    = true;
    bool   show_angle = true;
    bool   show_fit   = true;
    bool   show_ref   = false;
    double ref_a      = 0.0;
    double ref_b      = 0.0;

    // render() is called once per frame inside the ImGui render loop.
    void render() override;

    void load_settings(const nlohmann::json& j);
    nlohmann::json save_settings() const;
};
