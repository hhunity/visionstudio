#pragma once
#include <array>
#include <initializer_list>
#include <vector>
#include <imgui.h>
#include "gui/image_viewer.h"
#include "gui/compare_viewer.h"
#include "io/overlay_io.h"

class profile_panel {
public:
    bool visible = false;

    // Call once after all stable references are known (before the render loop).
    void init(const image_viewer*            single_viewer,
              const compare_viewer*          compare,
              const std::vector<roi_group>*  overlays,
              const std::vector<roi_group>*  left_overlays);

    // Call once per frame inside the ImGui render loop.
    void render(bool use_single, float viewer_w, float viewer_h);

private:
    struct series_entry { const image_data* img; ImU32 color; int cursor; };

    void draw_profile(const char* plot_id, bool is_x, int fixed,
                      std::initializer_list<series_entry> series,
                      float gw, float gh,
                      int vis_min = -1, int vis_max = -1);

    const image_viewer*            single_viewer_ = nullptr;
    const compare_viewer*          compare_       = nullptr;
    const std::vector<roi_group>*  overlays_      = nullptr;
    const std::vector<roi_group>*  left_overlays_ = nullptr;
};
