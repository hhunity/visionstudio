#pragma once
#include <array>
#include <initializer_list>
#include <imgui.h>
#include "gui/viewer_context.h"

class profile_panel {
public:
    bool visible = false;

    // Call once per frame inside the ImGui render loop.
    void render(const viewer_context& ctx);

private:
    struct series_entry { const image_data* img; ImU32 color; int cursor; };

    void draw_profile(const char* plot_id, bool is_x, int fixed,
                      std::initializer_list<series_entry> series,
                      float gw, float gh,
                      int vis_min = -1, int vis_max = -1);
};
