#include "gui/capture_panel.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <glad/glad.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.hpp>

void capture_panel::init(
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
) {
    cur_sse_             = cur_sse;
    capturing_           = capturing;
    cap_cfg_             = cap_cfg;
    conn_buf_            = conn_buf;
    capture_mode_        = capture_mode;
    vmode_               = vmode;
    image_acquisition_   = image_acquisition;
    live_image_          = live_image;
    auto_detect_         = auto_detect;
    ref_img_path_        = ref_img_path;
    left_loader_         = left_loader;
    show_camera_config_  = show_camera_config;
    show_connect_config_ = show_connect_config;
    capture_cfg_tab_     = capture_cfg_tab;
    connect_cfg_tab_     = connect_cfg_tab;
    compare_             = compare;
    cam_info_            = cam_info;
    cam_info_future_     = cam_info_future;
    preview_tex_         = preview_tex;
    preview_tex_w_       = preview_tex_w;
    preview_tex_h_       = preview_tex_h;
    log_                 = log;
    right_loader_        = right_loader;
}

void capture_panel::clamp_drag_pre_frame() {
    if (min_y_ <= 0.0f) return;

    ImGuiContext& g   = *ImGui::GetCurrentContext();
    ImGuiWindow*  win = ImGui::FindWindowByName("Capture Control##cap_panel");
    if (!win) return;

    // Clamp drag: window_pos.y = mouse_y - ActiveIdClickOffset.y
    // Keeping offset <= mouse_y - min_y ensures window_pos.y >= min_y.
    if (g.MovingWindow == win) {
        const float max_offset_y = g.IO.MousePos.y - min_y_;
        if (g.ActiveIdClickOffset.y > max_offset_y)
            g.ActiveIdClickOffset.y = max_offset_y;
    }

    // Non-drag correction (e.g. restored from imgui.ini above min_y).
    if (win->Pos.y < min_y_)
        ImGui::SetNextWindowPos({win->Pos.x, min_y_}, ImGuiCond_Always);
}

void capture_panel::poll_events() {
    if (!cap_cli_.has_value()) return;

    while (auto ev = cap_cli_->poll_server_event()) {
        if (std::get_if<evt_connected>(&*ev)) {
            *cur_sse_ = sse_state::connected;
            log_->add("INFO", "Connected to server");
        } else if (std::get_if<evt_disconnected>(&*ev)) {
            *cur_sse_   = sse_state::disconnected;
            *capturing_ = false;
            log_->add("INFO", "Server disconnected");
        } else if (auto* e = std::get_if<evt_error>(&*ev)) {
            *cur_sse_ = sse_state::error;
            log_->add("ERROR", ("Server error: " + e->message).c_str());
        } else if (auto* e = std::get_if<evt_capture_done>(&*ev)) {
            *capturing_ = false;
            if (*capture_mode_ == 0) {
                // Single mode: replace single viewer image
                left_loader_->start(e->path);
            } else if (*capture_mode_ == 1) {
                // Compare mode: replace both
                compare_->unload_left();
                compare_->unload_right();
                left_loader_->start(e->path);
            } else {
                // Mode 2: keep left as reference, replace right only
                compare_->unload_right();
                right_loader_->start(e->path);
            }
            log_->add("INFO", ("Capture done: " + e->path).c_str());
        } else if (auto* e = std::get_if<evt_config_updated>(&*ev)) {
            *cap_cfg_  = e->cfg;
            *conn_buf_ = make_conn_edit(*cap_cfg_);
            capture_config::save("visionstudio.json", *cap_cfg_);
            log_->add("INFO", "Config updated by server");
        } else if (auto* e = std::get_if<evt_camera_info>(&*ev)) {
            *cam_info_ = std::move(e->groups);
        }
    }

    // Poll camera info future (Refresh button result)
    if (cam_info_future_->valid() &&
        cam_info_future_->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        *cam_info_ = cam_info_future_->get();
}

bool capture_panel::is_preview_active() const {
    return cap_cli_.has_value() && cap_cli_->is_preview_active();
}

bool capture_panel::poll_preview_frame(preview_frame& out) {
    if (!cap_cli_.has_value()) return false;
    return cap_cli_->poll_preview_frame(out);
}

void capture_panel::cancel_connect() {
    if (cap_cli_.has_value()) cap_cli_->disconnect();
    if (cur_sse_) *cur_sse_ = sse_state::disconnected;
}

void capture_panel::render(float min_y) {
    // Poll SSE events first.
    poll_events();

    if (!visible) return;

    // Store min_y for use in clamp_drag_pre_frame() next frame.
    min_y_ = min_y;

    // First-frame placement when window doesn't exist yet.
    if (min_y > 0.0f && !ImGui::FindWindowByName("Capture Control##cap_panel"))
        ImGui::SetNextWindowPos({20.0f, min_y}, ImGuiCond_FirstUseEver);

    ImGui::SetNextWindowSize({220.0f, 600.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Capture Control##cap_panel", &visible)) { ImGui::End(); return; }

    // SSE status indicator
    const char* sse_label = "";
    ImVec4      sse_col   = {1, 1, 1, 1};
    switch (*cur_sse_) {
    case sse_state::disconnected: sse_label = "Disconnected"; sse_col = {0.6f, 0.6f, 0.6f, 1}; break;
    case sse_state::connecting:   sse_label = "Connecting..."; sse_col = {1, 0.8f, 0, 1};       break;
    case sse_state::connected:    sse_label = "Connected";    sse_col = {0.2f, 1, 0.4f, 1};    break;
    case sse_state::error:        sse_label = "Error";        sse_col = {1, 0.3f, 0.3f, 1};    break;
    }
    ImGui::TextColored(sse_col, "SSE: %s", sse_label);
    ImGui::Separator();

    // Connection settings (collapsible)
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
    const bool conn_open = ImGui::CollapsingHeader("Connect Settings");
    ImGui::PopStyleColor(3);
    if (conn_open) {
        ImGui::BeginDisabled(*cur_sse_ == sse_state::connected);
        bool conn_changed = false;
        const float label_col_w = ImGui::CalcTextSize("Timeout(ms)").x
                                + ImGui::GetStyle().ItemSpacing.x;
        auto labeled = [&](const char* label, auto fn) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (fn()) conn_changed = true;
        };
        constexpr ImGuiTableFlags kTblFlags = ImGuiTableFlags_None;
        if (ImGui::BeginTable("##conn_host", 2, kTblFlags)) {
            ImGui::TableSetupColumn("##lbl1", ImGuiTableColumnFlags_WidthFixed, label_col_w);
            ImGui::TableSetupColumn("##val1", ImGuiTableColumnFlags_WidthStretch);
            labeled("Host", [&]{ return ImGui::InputText("##host", conn_buf_->host, sizeof(conn_buf_->host)); });
            labeled("Port", [&]{ return ImGui::InputInt ("##port", &conn_buf_->port, 0); });
            ImGui::EndTable();
        }
        ImGui::Separator();
        if (ImGui::BeginTable("##conn_paths", 2, kTblFlags)) {
            ImGui::TableSetupColumn("##lbl2", ImGuiTableColumnFlags_WidthFixed, label_col_w);
            ImGui::TableSetupColumn("##val2", ImGuiTableColumnFlags_WidthStretch);
            labeled("Connect",    [&]{ return ImGui::InputText("##conn_path",  conn_buf_->connect_path,    sizeof(conn_buf_->connect_path)); });
            labeled("Start",      [&]{ return ImGui::InputText("##start_path", conn_buf_->start_path,      sizeof(conn_buf_->start_path)); });
            labeled("Stop",       [&]{ return ImGui::InputText("##stop_path",  conn_buf_->stop_path,       sizeof(conn_buf_->stop_path)); });
            labeled("Disconnect", [&]{ return ImGui::InputText("##disc_path",  conn_buf_->disconnect_path, sizeof(conn_buf_->disconnect_path)); });
            labeled("SSE",        [&]{ return ImGui::InputText("##sse_path",   conn_buf_->sse_path,        sizeof(conn_buf_->sse_path)); });
            labeled("Timeout(ms)",[&]{ return ImGui::InputInt ("##timeout",    &conn_buf_->timeout_ms,     0); });
            ImGui::EndTable();
        }
        if (conn_changed) {
            cap_cfg_->host            = conn_buf_->host;
            cap_cfg_->port            = conn_buf_->port;
            cap_cfg_->connect_path    = conn_buf_->connect_path;
            cap_cfg_->start_path      = conn_buf_->start_path;
            cap_cfg_->stop_path       = conn_buf_->stop_path;
            cap_cfg_->disconnect_path = conn_buf_->disconnect_path;
            cap_cfg_->sse_path        = conn_buf_->sse_path;
            cap_cfg_->timeout_ms      = conn_buf_->timeout_ms;
            capture_config::save("visionstudio.json", *cap_cfg_);
        }
        if (!connect_cfg_tab_->path.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Connect Config File");
            const auto& p   = connect_cfg_tab_->path;
            const auto  pos = p.find_last_of("/\\");
            const std::string fname = (pos == std::string::npos) ? p : p.substr(pos + 1);
            if (ImGui::Button(fname.c_str(), {-1, 0}))
                *show_connect_config_ = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();

    // Connection buttons (blue)
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.15f, 0.45f, 0.75f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.22f, 0.58f, 0.90f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.10f, 0.32f, 0.55f, 1.0f});
    ImGui::BeginDisabled(*cur_sse_ != sse_state::disconnected &&
                         *cur_sse_ != sse_state::error);
    if (ImGui::Button("Connect", {-1, 0})) {
        cap_cli_.emplace(*cap_cfg_);
        cap_cli_->connect();
        *cur_sse_ = sse_state::connecting;
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(*cur_sse_ != sse_state::connected);
    if (ImGui::Button("Disconnect", {-1, 0})) {
        if (cap_cli_.has_value()) cap_cli_->disconnect();
        *capturing_ = false;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::Separator();

    // Capture Settings (collapsible)
#ifndef NDEBUG
    constexpr bool cap_settings_disabled = false;
#else
    const bool cap_settings_disabled = *cur_sse_ != sse_state::connected;
#endif
    ImGui::BeginDisabled(cap_settings_disabled);
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
    const bool cap_settings_open = ImGui::CollapsingHeader("Capture Settings");
    ImGui::PopStyleColor(3);
    if (cap_settings_open) {
        constexpr const char* kCaptureModes[] = {"Single", "Compare", "Compare (keep left)"};
        ImGui::TextDisabled("View Mode");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const int prev_cap_mode = *capture_mode_;
        if (ImGui::Combo("##cap_mode", capture_mode_, kCaptureModes, 3)) {
            if (prev_cap_mode == 1 && *capture_mode_ == 0) {
                ref_img_path_->clear();
                compare_->unload_left();
            }
            *vmode_ = (*capture_mode_ == 0) ? view_mode::single : view_mode::compare;
        }
        ImGui::Separator();

        if (ImGui::BeginTable("##sw_table", 2, ImGuiTableFlags_None)) {
            const float sw_lbl_w = ImGui::CalcTextSize("Image Acquisition").x
                                 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::TableSetupColumn("##sw_lbl", ImGuiTableColumnFlags_WidthFixed, sw_lbl_w);
            ImGui::TableSetupColumn("##sw_val", ImGuiTableColumnFlags_WidthFixed);
            auto sw_row = [&](const char* label, const char* id, bool* val) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", label);
                ImGui::TableSetColumnIndex(1);
                // toggle_switch widget inline.
                const float h = ImGui::GetFrameHeight() * 0.85f;
                const float w = h * 1.8f;
                const float r = h * 0.5f;
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton(id, {w, h});
                if (ImGui::IsItemClicked()) *val = !*val;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float t  = *val ? 1.0f : 0.0f;
                const ImU32 track = ImGui::IsItemHovered()
                    ? (*val ? IM_COL32( 80,190, 80,255) : IM_COL32(190,190,190,255))
                    : (*val ? IM_COL32( 66,170, 66,255) : IM_COL32(160,160,160,255));
                dl->AddRectFilled(p, {p.x + w, p.y + h}, track, r);
                dl->AddCircleFilled({p.x + r + t * (w - r * 2.0f), p.y + r},
                                    r - 2.0f, IM_COL32(255,255,255,255));
            };
            sw_row("Image Acquisition", "##acq",  image_acquisition_);
            sw_row("Live Image",        "##live", live_image_);
            sw_row("Auto Detect",       "##ad",   auto_detect_);
            ImGui::EndTable();
        }

        ImGui::BeginDisabled(*vmode_ != view_mode::compare);
        ImGui::TextDisabled("Ref Img");
        {
            const auto pos = ref_img_path_->find_last_of("/\\");
            const std::string fname = ref_img_path_->empty()
                ? "(none)"
                : (pos == std::string::npos ? *ref_img_path_ : ref_img_path_->substr(pos + 1));
            if (ImGui::Button(fname.c_str(), {-1, 0})) {
                constexpr nfdfilteritem_t kTiffFilter[] = {{"TIFF Image", "tiff,tif"}};
                nfdchar_t* out = nullptr;
                if (NFD::OpenDialog(out, kTiffFilter, 1) == NFD_OKAY) {
                    *ref_img_path_ = out;
                    NFD::FreePath(out);
                    left_loader_->start(*ref_img_path_);
                }
            }
        }
        ImGui::EndDisabled();

        // Guide lines
        ImGui::Separator();
        ImGui::TextDisabled("Guide Lines  (-1 = off)");
        if (ImGui::BeginTable("##guide_table", 2, ImGuiTableFlags_None)) {
            const float gl_lbl_w = ImGui::CalcTextSize("targetx").x
                                 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::TableSetupColumn("##gl_lbl", ImGuiTableColumnFlags_WidthFixed, gl_lbl_w);
            ImGui::TableSetupColumn("##gl_val", ImGuiTableColumnFlags_WidthStretch);
            auto gl_row = [&](const char* label, const char* id, int* val) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", label);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt(id, val, 0))
                    capture_config::save("visionstudio.json", *cap_cfg_);
            };
            gl_row("basex",   "##basex",   &cap_cfg_->basex);
            gl_row("targetx", "##targetx", &cap_cfg_->targetx);
            gl_row("starty",  "##starty",  &cap_cfg_->starty);
            gl_row("liney",   "##liney",   &cap_cfg_->liney);
            ImGui::EndTable();
        }

        // Capture config files
        ImGui::Separator();
        ImGui::TextDisabled("Capture Config Files");
        if (!capture_cfg_tab_->path.empty()) {
            const auto& fpath = capture_cfg_tab_->path;
            const auto  pos   = fpath.find_last_of("/\\");
            const std::string fname = (pos == std::string::npos) ? fpath : fpath.substr(pos + 1);
            if (ImGui::Button(fname.c_str(), {-1, 0}))
                *show_camera_config_ = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    // Capture buttons
    ImGui::BeginDisabled(*cur_sse_ != sse_state::connected || *capturing_);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.18f, 0.55f, 0.18f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.25f, 0.70f, 0.25f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.12f, 0.40f, 0.12f, 1.0f});
    if (ImGui::Button("Start Capture", {-1, 0})) {
        if (cap_cli_.has_value()) {
            cap_cli_->start_capture();
            *capturing_ = true;
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(*cur_sse_ != sse_state::connected || !*capturing_);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.60f, 0.15f, 0.15f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.78f, 0.20f, 0.20f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.45f, 0.10f, 0.10f, 1.0f});
    if (ImGui::Button("Stop Capture", {-1, 0})) {
        if (cap_cli_.has_value()) {
            cap_cli_->stop_capture();
            *capturing_ = false;
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    // Preview
    ImGui::Separator();
    const bool preview_on = is_preview_active();
    ImGui::BeginDisabled(preview_on);
    if (ImGui::Checkbox("Raw", &cap_cfg_->preview_raw))
        capture_config::save("visionstudio.json", *cap_cfg_);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(*cur_sse_ != sse_state::connected);
    if (!preview_on) {
        if (ImGui::Button("Start Preview", {-1, 0})) {
            if (*preview_tex_ != 0) {
                uint32_t tex = *preview_tex_;
                glDeleteTextures(1, &tex);
                *preview_tex_   = 0;
                *preview_tex_w_ = 0;
                *preview_tex_h_ = 0;
            }
            if (cap_cli_.has_value()) cap_cli_->start_preview();
        }
    } else {
        if (ImGui::Button("Stop Preview", {-1, 0})) {
            if (cap_cli_.has_value()) cap_cli_->stop_preview();
        }
    }
    ImGui::EndDisabled();

    // Camera Info
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
    const bool cam_info_open = ImGui::CollapsingHeader("Camera Info");
    ImGui::PopStyleColor(3);
    if (cam_info_open) {
        const bool fetching = cam_info_future_->valid() &&
            cam_info_future_->wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        ImGui::BeginDisabled(fetching || *cur_sse_ != sse_state::connected);
        if (ImGui::SmallButton(fetching ? "Fetching..." : "Refresh Info")) {
            if (cap_cli_.has_value()) {
                capture_client* cli = &*cap_cli_;
                *cam_info_future_ = std::async(std::launch::async,
                    [cli]{ return cli->fetch_camera_info(); });
            }
        }
        ImGui::EndDisabled();

        for (auto& g : *cam_info_) {
            if (ImGui::CollapsingHeader(g.label.c_str())) {
                if (ImGui::BeginTable(g.label.c_str(), 2,
                        ImGuiTableFlags_SizingFixedFit |
                        ImGuiTableFlags_RowBg          |
                        ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 0.5f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                    for (auto& p : g.params) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const float row_top = ImGui::GetCursorScreenPos().y;
                        const ImVec2 mouse  = ImGui::GetMousePos();
                        const bool row_hov  = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                                              mouse.y >= row_top &&
                                              mouse.y <  row_top + ImGui::GetTextLineHeightWithSpacing();
                        if (p.rw_type == cam_param_rw::readwrite)
                            ImGui::TextUnformatted(p.name.c_str());
                        else
                            ImGui::TextDisabled("%s", p.name.c_str());
                        ImGui::TableSetColumnIndex(1);

                        if (p.rw_type == cam_param_rw::writeonly) {
                            ImGui::TextDisabled("---");
                        } else if (p.rw_type == cam_param_rw::readonly) {
                            const std::string disp = p.unit.empty()
                                ? p.value : p.value + " " + p.unit;
                            ImGui::TextDisabled("%s", disp.c_str());
                        } else {
                            const std::string key      = g.label + "/" + p.name;
                            const std::string popup_id = "##ep_" + key;

                            if (cam_edit_key == key) {
                                if (p.type == cam_param_type::bool_ ||
                                    p.type == cam_param_type::enum_) {
                                    if (ImGui::BeginPopup(popup_id.c_str())) {
                                        for (const auto& opt : p.options) {
                                            if (ImGui::Selectable(opt.c_str(), opt == p.value)) {
                                                p.value = opt;
                                                cam_edit_key.clear();
                                                ImGui::CloseCurrentPopup();
                                                if (cap_cli_.has_value())
                                                    cap_cli_->update_param(p.name, p.value);
                                            }
                                        }
                                        ImGui::EndPopup();
                                    } else {
                                        cam_edit_key.clear();
                                    }
                                    const std::string disp = p.unit.empty()
                                        ? p.value : p.value + " " + p.unit;
                                    ImGui::TextUnformatted(disp.c_str());
                                } else {
                                    if (cam_edit_focus) {
                                        ImGui::SetKeyboardFocusHere();
                                        cam_edit_focus = false;
                                    }
                                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                                    const bool enter = ImGui::InputText(
                                        ("##camedit_" + p.name).c_str(),
                                        cam_edit_buf, sizeof(cam_edit_buf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
                                    if (enter || ImGui::IsItemDeactivated()) {
                                        const std::string raw = cam_edit_buf;
                                        if (p.type == cam_param_type::int_) {
                                            try {
                                                int v = std::stoi(raw);
                                                if (!p.min.empty()) v = std::max(v, std::stoi(p.min));
                                                if (!p.max.empty()) v = std::min(v, std::stoi(p.max));
                                                p.value = std::to_string(v);
                                            } catch (...) {}
                                        } else if (p.type == cam_param_type::float_) {
                                            try {
                                                float v = std::stof(raw);
                                                if (!p.min.empty()) v = std::max(v, std::stof(p.min));
                                                if (!p.max.empty()) v = std::min(v, std::stof(p.max));
                                                char fb[64];
                                                std::snprintf(fb, sizeof(fb), "%g",
                                                              static_cast<double>(v));
                                                p.value = fb;
                                            } catch (...) {}
                                        } else {
                                            p.value = raw;
                                        }
                                        cam_edit_key.clear();
                                        if (cap_cli_.has_value())
                                            cap_cli_->update_param(p.name, p.value);
                                    }
                                }
                            } else {
                                const std::string disp = p.unit.empty()
                                    ? p.value : p.value + " " + p.unit;
                                ImGui::TextUnformatted(disp.c_str());
                                if (row_hov &&
                                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                    cam_edit_key = key;
                                    if (p.type == cam_param_type::bool_ ||
                                        p.type == cam_param_type::enum_) {
                                        ImGui::OpenPopup(popup_id.c_str());
                                    } else {
                                        std::strncpy(cam_edit_buf, p.value.c_str(),
                                                     sizeof(cam_edit_buf) - 1);
                                        cam_edit_buf[sizeof(cam_edit_buf) - 1] = '\0';
                                        cam_edit_focus = true;
                                    }
                                }
                            }
                        }
                        if (row_hov && cam_edit_key.empty() &&
                            (!p.description.empty() || !p.min.empty() || !p.initial.empty())) {
                            ImGui::BeginTooltip();
                            if (!p.description.empty()) ImGui::TextUnformatted(p.description.c_str());
                            if (!p.min.empty() || !p.max.empty())
                                ImGui::Text("Range: %s - %s", p.min.c_str(), p.max.c_str());
                            if (!p.initial.empty())
                                ImGui::Text("Default: %s %s", p.initial.c_str(), p.unit.c_str());
                            ImGui::EndTooltip();
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    ImGui::End();
}
