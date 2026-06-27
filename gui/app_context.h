#pragma once
#include <future>
#include <string>
#include <vector>
#include "capture/capture_client.h"
#include "gui/app_types.h"

// Forward declarations — include the actual headers in .cpp files
class  image_viewer;
class  compare_viewer;
class  log_panel;
struct async_loader;
struct capture_config;
struct conn_edit;
struct config_tab;
struct cam_info_group;
struct roi_group;

struct app_context {
    // ---- Viewer (stable pointers) ----
    image_viewer*                 single_viewer   = nullptr;
    compare_viewer*               compare         = nullptr;
    std::vector<roi_group>*       overlays        = nullptr;
    std::vector<roi_group>*       left_overlays   = nullptr;

    // ---- Per-frame viewer state ----
    bool  use_single = true;
    float viewer_w   = 0.0f;
    float viewer_h   = 0.0f;

    // ---- Capture panel clamp ----
    float min_y = 0.0f;

    // ---- Loaders ----
    async_loader* left_loader  = nullptr;
    async_loader* right_loader = nullptr;

    // ---- Capture / SSE state ----
    sse_state*                                cur_sse             = nullptr;
    bool*                                     capturing           = nullptr;
    capture_config*                           cap_cfg             = nullptr;
    conn_edit*                                conn_buf            = nullptr;
    int*                                      capture_mode        = nullptr;
    view_mode*                                vmode               = nullptr;
    bool*                                     image_acquisition   = nullptr;
    bool*                                     live_image          = nullptr;
    bool*                                     auto_detect         = nullptr;
    std::string*                              ref_img_path        = nullptr;
    bool*                                     show_camera_config  = nullptr;
    bool*                                     show_connect_config = nullptr;
    config_tab*                               capture_cfg_tab     = nullptr;
    config_tab*                               connect_cfg_tab     = nullptr;
    std::vector<cam_info_group>*              cam_info            = nullptr;
    std::future<std::vector<cam_info_group>>* cam_info_future     = nullptr;
    uint32_t*                                 preview_tex         = nullptr;
    int*                                      preview_tex_w       = nullptr;
    int*                                      preview_tex_h       = nullptr;

    // ---- Shared services ----
    log_panel* log = nullptr;
};
