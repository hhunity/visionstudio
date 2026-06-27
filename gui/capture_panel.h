#pragma once
#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <vector>
#include <imgui.h>
#include "capture/capture_client.h"
#include "gui/app_context.h"
#include "gui/app_types.h"
#include "gui/panel_base.h"
#include "util/capture_config.h"

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

class capture_panel : public panel_base {
public:
    // visible is inherited from panel_base (default false); override initial value
    capture_panel() { visible = true; }

    // Init from app_context (overrides panel_base::init).
    void init(app_context* ctx) override;

    // Called each frame before any ImGui::Begin() — clamps drag position.
    void pre_frame() override;

    // Poll SSE events from the capture client and update state.
    void poll_events();

    void render() override;

    bool is_preview_active() const;
    bool poll_preview_frame(preview_frame& out);
    void cancel_connect();

    // Public so clamp_drag_pre_frame can be kept for backward-compat (called via pre_frame).
    void clamp_drag_pre_frame();

private:
    // Owned capture client (created lazily in render/connect).
    std::optional<capture_client> cap_cli_;

    // Per-panel state.
    std::string cam_edit_key;
    char        cam_edit_buf[256] = {};
    bool        cam_edit_focus    = false;
    float       min_y_            = 0.0f;  // updated each render(), used next frame
};
