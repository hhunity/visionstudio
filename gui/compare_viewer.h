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

    bool is_split() const { return is_split_; }

    // Render two panels side by side.
    // width/height of 0 means "fill available space".
    void render(float width, float height);

    bool        show_grid        = false;
    int         grid_spacing     = 100;
    bool        show_coordinates = true;
    bool        show_minimap     = true;
    bool        sync_views       = true;
    bool        diff_mode        = false;  // right panel shows |left - right|
    float       diff_amplify     = 1.0f;   // multiply diff values to enhance subtle differences
    float       split_ratio      = 0.5f;   // split position (0.01-0.99); only used in split mode
    std::string left_label;
    std::string right_label;

private:
    void compute_diff();
    void update_right_viewer();  // reload right viewer with diff or original
    void apply_split();          // slice split_src_ at split_ratio and reload both viewers

    image_viewer left_viewer_;
    image_viewer right_viewer_;
    view_state   shared_state_;

    image_data right_orig_;       // original right image (needed to restore when diff is toggled off)
    image_data diff_data_;        // computed diff image
    bool       diff_applied_    = false;
    float      amplify_applied_ = 1.0f;

    image_data split_src_;              // original full image when in split mode
    bool       is_split_           = false;
    float      split_ratio_applied_ = -1.0f;  // last ratio used; -1 forces initial apply
};
