#pragma once
#include <array>
#include <initializer_list>
#include <vector>
#include <imgui.h>
#include "gui/panel_base.h"
#include "external/cpplib/io/tiff_io.h"

class profile_panel : public panel_base {
public:
    // visible is inherited from panel_base

    // render() is called once per frame inside the ImGui render loop.
    void render() override;

private:
    struct series_entry { const image_data* img; ImU32 color; int cursor; };

    void draw_profile(const char* plot_id, bool is_x, int fixed,
                      std::initializer_list<series_entry> series,
                      float gw, float gh,
                      int vis_min = -1, int vis_max = -1);
};
