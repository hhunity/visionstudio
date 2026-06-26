#pragma once
#include <imgui.h>
#include <optional>
#include <vector>

class measure_tool {
public:
    static constexpr int kMaxEntries = 5;

    bool visible = false;

    // 1st click: set pt1 of new measurement
    // 2nd click: set pt2, complete the measurement
    void handle_click(int img_x, int img_y);

    void reset();

    // hover_img_x/y: image coords under cursor (-1 = not hovering, for rubber-band)
    void render_overlay(ImDrawList* dl,
                        ImVec2 canvas_pos, ImVec2 canvas_size,
                        float zoom, float pan_x, float pan_y,
                        int hover_img_x = -1, int hover_img_y = -1);

    void render_panel();

private:
    struct entry {
        float pt1_x = 0, pt1_y = 0;
        float pt2_x = 0, pt2_y = 0;
    };

    std::vector<entry>   entries_;   // completed measurements (up to kMaxEntries)
    std::optional<entry> pending_;   // pt1 set, waiting for pt2

    static const ImU32 kColors[kMaxEntries];

    ImVec2 to_screen(float img_x, float img_y,
                     ImVec2 canvas_pos, float zoom,
                     float pan_x, float pan_y) const;

    void draw_entry(ImDrawList* dl, const entry& e, ImU32 col,
                    ImVec2 canvas_pos, float zoom,
                    float pan_x, float pan_y) const;
};
