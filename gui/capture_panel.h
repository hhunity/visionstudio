#pragma once
#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <vector>
#include <imgui.h>
#include "capture/capture_client.h"
#include "gui/app_types.h"
#include "gui/compare_viewer.h"
#include "gui/log_panel.h"
#include "util/async_loader.h"
#include "util/capture_config.h"
#include "util/config_tab.h"

// Editable copy of connection settings (char buffers for ImGui InputText).
struct conn_edit {
    char host[128]            = {};
    int  port                 = 8080;
    char connect_path[64]     = {};
    char start_path[64]       = {};
    char stop_path[64]        = {};
    char disconnect_path[64]  = {};
    char sse_path[64]         = {};
    int  timeout_ms           = 5000;
};

inline conn_edit make_conn_edit(const capture_config& c) {
    conn_edit e{};
    std::strncpy(e.host,            c.host.c_str(),            sizeof(e.host) - 1);
    e.port = c.port;
    std::strncpy(e.connect_path,    c.connect_path.c_str(),    sizeof(e.connect_path) - 1);
    std::strncpy(e.start_path,      c.start_path.c_str(),      sizeof(e.start_path) - 1);
    std::strncpy(e.stop_path,       c.stop_path.c_str(),       sizeof(e.stop_path) - 1);
    std::strncpy(e.disconnect_path, c.disconnect_path.c_str(), sizeof(e.disconnect_path) - 1);
    std::strncpy(e.sse_path,        c.sse_path.c_str(),        sizeof(e.sse_path) - 1);
    e.timeout_ms = c.timeout_ms;
    return e;
}

class capture_panel {
public:
    bool visible = true;

    // Call once after all stable references are known (before the render loop).
    void init(
        sse_state*                                cur_sse,
        bool*                                     capturing,
        capture_config*                           cap_cfg,
        conn_edit*                                conn_buf,
        int*                                      capture_mode,
        view_mode*                                vmode,
        bool*                                     image_acquisition,
        bool*                                     live_image,
        bool*                                     auto_detect,
        std::string*                              ref_img_path,
        async_loader*                             left_loader,
        bool*                                     show_camera_config,
        bool*                                     show_connect_config,
        config_tab*                               capture_cfg_tab,
        config_tab*                               connect_cfg_tab,
        compare_viewer*                           compare,
        std::vector<cam_info_group>*              cam_info,
        std::future<std::vector<cam_info_group>>* cam_info_future,
        uint32_t*                                 preview_tex,
        int*                                      preview_tex_w,
        int*                                      preview_tex_h,
        log_panel*                                log,
        async_loader*                             right_loader
    );

    // Call this BEFORE the root ImGui::Begin() each frame so the drag clamp
    // takes effect before ImGui processes window movement.
    void clamp_drag_pre_frame();

    // Poll SSE events from the capture client and update state.
    void poll_events();

    void render(float min_y);

    bool is_preview_active() const;
    bool poll_preview_frame(preview_frame& out);
    void cancel_connect();

private:
    // Owned capture client (created lazily in render/connect).
    std::optional<capture_client> cap_cli_;

    // Stable pointers set once via init().
    sse_state*                                cur_sse_            = nullptr;
    bool*                                     capturing_          = nullptr;
    capture_config*                           cap_cfg_            = nullptr;
    conn_edit*                                conn_buf_           = nullptr;
    int*                                      capture_mode_       = nullptr;
    view_mode*                                vmode_              = nullptr;
    bool*                                     image_acquisition_  = nullptr;
    bool*                                     live_image_         = nullptr;
    bool*                                     auto_detect_        = nullptr;
    std::string*                              ref_img_path_       = nullptr;
    async_loader*                             left_loader_        = nullptr;
    bool*                                     show_camera_config_ = nullptr;
    bool*                                     show_connect_config_= nullptr;
    config_tab*                               capture_cfg_tab_    = nullptr;
    config_tab*                               connect_cfg_tab_    = nullptr;
    compare_viewer*                           compare_            = nullptr;
    std::vector<cam_info_group>*              cam_info_           = nullptr;
    std::future<std::vector<cam_info_group>>* cam_info_future_    = nullptr;
    uint32_t*                                 preview_tex_        = nullptr;
    int*                                      preview_tex_w_      = nullptr;
    int*                                      preview_tex_h_      = nullptr;
    log_panel*                                log_                = nullptr;
    async_loader*                             right_loader_       = nullptr;

    // Per-panel state.
    std::string cam_edit_key;
    char        cam_edit_buf[256] = {};
    bool        cam_edit_focus    = false;
    float       min_y_            = 0.0f;  // updated each render(), used next frame
};
