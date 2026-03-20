#pragma once
#include <string>
#include "gui/image_viewer.h"
#include "util/image_data.h"

// Side-by-side image comparison viewer with optional synchronized zoom/pan.
class compare_viewer {
public:
    compare_viewer();

    bool load_left(const image_data& img);
    bool load_right(const image_data& img);

    // Render two panels side by side.
    // width/height of 0 means "fill available space".
    void render(float width, float height);

    bool        show_grid        = false;
    int         grid_spacing     = 100;
    bool        show_coordinates = true;
    bool        sync_views       = true;
    std::string left_label;   // displayed above the left panel (e.g. file path)
    std::string right_label;  // displayed above the right panel

private:
    image_viewer left_viewer_;
    image_viewer right_viewer_;
    view_state   shared_state_;
};
