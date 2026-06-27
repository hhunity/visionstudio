#pragma once
#include <vector>
#include <imgui.h>
#include <implot.h>
#include <nlohmann/json.hpp>
#include "gui/image_viewer.h"
#include "gui/compare_viewer.h"
#include "io/overlay_io.h"

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

    // Call once after all stable references are known (before the render loop).
    // single_viewer / compare are non-const because overlay_group_visibility is mutable.
    void init(image_viewer*                 single_viewer,
              compare_viewer*               compare,
              const std::vector<roi_group>* overlays,
              const std::vector<roi_group>* left_overlays);

    // Call once per frame inside the ImGui render loop.
    void render(bool use_single);

    void load_settings(const nlohmann::json& j);
    nlohmann::json save_settings() const;

private:
    image_viewer*                 single_viewer_ = nullptr;
    compare_viewer*               compare_       = nullptr;
    const std::vector<roi_group>* overlays_      = nullptr;
    const std::vector<roi_group>* left_overlays_ = nullptr;
};
