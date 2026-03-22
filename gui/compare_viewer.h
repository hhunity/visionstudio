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
    bool load_single(const image_data& img);  // load same image to both left and right
    bool load_split(const image_data& img);   // split image: left portion / right portion
    void unload_left();                        // clear the left panel

    bool is_split() const { return is_split_; }
    int  split_src_width() const { return split_src_.width; }

    // Overlays for compare mode (independent left/right).
    void set_left_overlays(std::vector<roi_entry> entries);
    void set_right_overlays(std::vector<roi_entry> entries);
    // Overlay for split mode: stores source and re-splits whenever split_x changes.
    void set_split_overlays(std::vector<roi_entry> entries);
    void clear_overlays();

    // Image data access for profile graphs.
    const image_data& get_left_image_data()  const;
    const image_data& get_right_image_data() const;

    // Combined hover info for the pixel panel (same data as the tooltip).
    struct combined_hover_info {
        bool                   valid      = false;
        int                    img_x      = 0, img_y = 0;
        float                  zoom       = 1.0f;
        std::array<uint8_t, 4> left_rgba  = {};
        std::array<uint8_t, 4> right_rgba = {};
    };
    const combined_hover_info& get_hover_info() const { return combined_hover_; }

    // Render two panels side by side.
    // width/height of 0 means "fill available space".
    void render(float width, float height);

    bool        show_grid        = false;
    int         grid_spacing     = 100;
    bool        show_coordinates = false;
    bool        show_minimap     = true;
    bool        show_overlays    = true;
    bool        show_crosshair   = false;
    bool        sync_views       = true;
    bool        diff_mode        = false;  // right panel shows |left - right|
    float       diff_amplify     = 1.0f;   // multiply diff values to enhance subtle differences
    int         split_x          = 0;      // split position in pixels; only used in split mode
    std::string left_label;
    std::string right_label;

private:
    void compute_diff();
    void update_right_viewer();    // reload right viewer with diff or original
    void apply_split();            // slice split_src_ at split_x and reload both viewers
    void apply_split_overlays();   // re-clip split_overlays_ at current split_x

    image_viewer left_viewer_;
    image_viewer right_viewer_;
    view_state   shared_state_;

    image_data right_orig_;       // original right image (needed to restore when diff is toggled off)
    image_data diff_data_;        // computed diff image
    bool       diff_applied_    = false;
    float      amplify_applied_ = 1.0f;

    image_data             split_src_;           // original full image when in split mode
    bool                   is_split_        = false;
    int                    split_x_applied_ = -1; // last pixel position applied; -1 forces initial apply

    std::vector<roi_entry> split_overlays_;       // source overlays for split mode
    combined_hover_info    combined_hover_;        // updated every frame in render()
};
