#pragma once
#include <vector>
#include "gui/image_viewer.h"
#include "gui/compare_viewer.h"
#include "io/overlay_io.h"

// Shared read-only snapshot passed from the main loop to all floating panels
// each frame. Add fields here when a new panel needs access to app state.
struct viewer_context {
    bool                           use_single    = true;
    const image_viewer*            single_viewer = nullptr;
    const compare_viewer*          compare       = nullptr;
    float                          viewer_w      = 0.0f;
    float                          viewer_h      = 0.0f;
    const std::vector<roi_group>*  overlays      = nullptr;
    const std::vector<roi_group>*  left_overlays = nullptr;
};
