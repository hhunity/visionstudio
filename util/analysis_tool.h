#pragma once
#include <string_view>
#include <imgui.h>
#include "util/image_data.h"

// Forward declarations to avoid gui <-> util circular dependency
struct view_state;

// Abstract interface for pluggable image analysis tools.
// Each tool is self-contained: it runs detection, owns results, and
// renders its own panel and overlay.
class analysis_tool {
public:
    virtual ~analysis_tool() = default;

    virtual std::string_view name() const = 0;

    // Run detection on the given image.  Call only when the image changes
    // or parameters are updated (not every frame).
    virtual void analyze(const image_data& img) = 0;

    // Render the ImGui parameter/results panel.  Called every frame when visible.
    virtual void render_panel() = 0;

    // Draw detection results on top of the image viewer canvas.
    // canvas_pos  : top-left screen position of the viewer canvas (from ImGui)
    // canvas_size : pixel dimensions of the viewer canvas
    // zoom, pan_x/y : current view_state values for image→screen coordinate mapping
    virtual void render_overlay(ImDrawList* dl,
                                ImVec2      canvas_pos,
                                ImVec2      canvas_size,
                                float       zoom,
                                float       pan_x,
                                float       pan_y) = 0;

    bool visible = false;
};
