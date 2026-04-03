#pragma once
#include <cstdint>
#include <imgui.h>
#include "util/image_data.h"
#include "io/overlay_io.h"

// Shared zoom/pan state — can be owned by the viewer or provided externally
// to synchronize multiple viewers.
struct view_state {
    float zoom  = 1.0f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
};

class image_viewer {
public:
    image_viewer();
    ~image_viewer();

    image_viewer(const image_viewer&)            = delete;
    image_viewer& operator=(const image_viewer&) = delete;

    // Upload image to GPU. Returns false if img is empty.
    // reset_view=false keeps the current zoom/pan (useful when re-slicing a split image).
    bool load_image(const image_data& img, bool reset_view = true);
    void unload_image();
    bool has_image() const { return !tiles_.empty(); }

    // Render the viewer canvas.
    //   id     : unique ImGui widget id string
    //   width  : canvas width  (0 = fill available)
    //   height : canvas height (0 = fill available)
    //   state  : optional external view_state for synchronization;
    //            uses internal state when nullptr.
    void render(const char* id, float width, float height,
                view_state* state = nullptr);

    // Fit the view so the image fills the given canvas dimensions.
    void fit_view(view_state& state, float canvas_w, float canvas_h) const;

    // Pixel under the mouse — updated every frame in render().
    struct hover_info {
        bool                   valid = false;
        int                    img_x = 0, img_y = 0;
        std::array<uint8_t, 4> rgba  = {};
        float                  zoom  = 1.0f;
    };
    const hover_info& get_hover_info() const { return last_hover_; }

    // Result of querying the pixel under the mouse cursor.
    struct mouse_query {
        bool                   valid = false;
        int                    img_x = 0, img_y = 0;
        std::array<uint8_t, 4> rgba  = {};
    };

    // Returns pixel info if the mouse is inside the given canvas rectangle.
    mouse_query query_mouse_pixel(const ImVec2& canvas_pos,
                                  const ImVec2& canvas_size,
                                  const view_state& state) const;

    // Sample a pixel by image coordinate.
    std::array<uint8_t, 4> pixel_at(int x, int y) const;

    // Access the internally owned view_state (used by compare_viewer when sync is off).
    const view_state& get_view_state() const { return owned_state_; }

    // Read-only access to the CPU image data (used by compare_viewer for diff).
    const image_data& get_image_data() const { return cpu_image_; }

    // Overlay: load ROI groups for heatmap display.
    void set_overlay_groups(std::vector<roi_group> groups);
    void clear_overlays();

    // Per-group visibility (index matches groups passed to set_overlay_groups).
    // uint8_t instead of bool to allow taking bool* for ImGui (vector<bool> is bit-packed).
    std::vector<uint8_t> overlay_group_visibility;

    size_t             overlay_group_count() const;
    const std::string& overlay_group_label(size_t i) const;

    // Display options
    bool  show_grid           = false;
    int   grid_spacing        = 100;   // spacing in image-space pixels
    bool  show_coordinates    = false;
    bool  show_minimap        = true;
    bool  show_overlays       = true;
    bool  show_crosshair      = false;
    // Minimap aspect ratio override: 0 = preserve image aspect; >0 = force this W/H ratio.
    // Useful for very elongated images (e.g. line-scan) where the natural minimap
    // becomes too thin to be useful.
    float minimap_force_aspect = 0.0f;

private:
    void create_texture(const image_data& img);
    void destroy_texture();

    void handle_input(const ImVec2& canvas_pos, const ImVec2& canvas_size,
                      view_state& state);
    void draw_content(ImDrawList* dl, const ImVec2& canvas_pos,
                      const ImVec2& canvas_size, const view_state& state) const;
    void draw_grid(ImDrawList* dl, const ImVec2& origin,
                   const ImVec2& size, const view_state& state) const;
    void draw_coordinate_tooltip(const ImVec2& canvas_pos,
                                 const view_state& state) const;
    void draw_minimap(ImDrawList* dl, const ImVec2& canvas_pos,
                      const ImVec2& canvas_size, const view_state& state) const;
    void draw_overlays(ImDrawList* dl, const ImVec2& canvas_pos,
                       const view_state& state) const;
    static void draw_dashed_line(ImDrawList* dl, ImVec2 a, ImVec2 b,
                                 ImU32 color, float thickness,
                                 float dash = 5.0f, float gap = 4.0f);

    // Vertical tiles: each covers rows [y0, y1) at full resolution.
    struct tex_tile { uint32_t id; int y0, y1; };
    std::vector<tex_tile> tiles_;
    // Minimap thumbnail (downscaled single texture when tiles_ has >1 entry).
    uint32_t minimap_tex_id_    = 0;
    bool     minimap_owns_tex_  = false; // true when minimap_tex_id_ is a separate allocation

    int img_w_ = 0;
    int img_h_ = 0;
    image_data cpu_image_;          // CPU copy kept for pixel inspection
    view_state             owned_state_;
    bool                   needs_fit_ = false;  // fit view on first render after load

    std::vector<roi_group> overlay_groups_;
    float                  overlay_max_mag_ = 1.0f; // for color normalization
    hover_info             last_hover_;
    bool                   minimap_dragging_ = false;
};
