#pragma once
#include <atomic>
#include <future>
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
    void unload_left();                        // clear the left panel

    // Overlays for compare mode (independent left/right).
    void set_left_overlay_groups(std::vector<roi_group> groups);
    void set_right_overlay_groups(std::vector<roi_group> groups);
    void clear_overlays();

    // Image data access for profile graphs.
    const image_data& get_left_image_data()  const;
    const image_data& get_right_image_data() const;

    // Dimensions of the unsliced source images (for offset slider range).
    int left_src_width()   const { return left_src_.width;   }
    int left_src_height()  const { return left_src_.height;  }
    int right_src_width()  const { return right_src_.width;  }
    int right_src_height() const { return right_src_.height; }

    // Viewer access for per-group overlay visibility UI.
    image_viewer& left_viewer_ref()  { return left_viewer_; }
    image_viewer& right_viewer_ref() { return right_viewer_; }

    // View state access for visible-range profiles.
    const view_state& get_view_state() const { return shared_state_; }

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

    bool        show_grid            = false;
    int         grid_spacing         = 100;
    bool        show_coordinates     = false;
    bool        show_minimap         = true;
    bool        show_left_overlays   = true;
    bool        show_right_overlays  = true;
    bool        show_crosshair       = false;
    float       minimap_force_aspect = 0.0f;
    bool        sync_views           = true;
    bool        diff_mode            = false;
    float       diff_amplify         = 1.0f;
    // Offset applied to each panel's start position (pixels).
    int         left_offset_x        = 0;
    int         left_offset_y        = 0;
    int         right_offset_x       = 0;
    int         right_offset_y       = 0;
    std::string left_label;
    std::string right_label;

private:
    void compute_diff();
    void update_right_viewer();
    void apply_left_offset();
    void apply_right_offset();
    void cancel_diff();  // wait for any in-flight diff to finish

    image_viewer left_viewer_;
    image_viewer right_viewer_;
    view_state   shared_state_;

    image_data left_src_;             // full left image before offset
    image_data right_src_;            // full right image before offset
    image_data right_orig_;           // offset-applied right slice (input to diff)
    image_data diff_data_;
    bool       diff_applied_          = false;
    float      amplify_applied_       = 1.0f;

    std::future<void>  diff_future_;
    std::atomic<int>   diff_rows_done_{0};
    std::atomic<bool>  diff_cancel_{false};
    int                diff_total_rows_ = 0;
    int        left_offset_applied_x  = 0;
    int        left_offset_applied_y  = 0;
    int        right_offset_applied_x = 0;
    int        right_offset_applied_y = 0;

    combined_hover_info combined_hover_;
};
