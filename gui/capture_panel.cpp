#include "gui/capture_panel.h"
#include "gui/app_context.h"
#include "gui/compare_viewer.h"
#include "gui/log_panel.h"
#include "util/async_loader.h"
#include "util/config_tab.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <glad/glad.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.hpp>

void capture_panel::init(app_context* ctx) {
    ctx_ = ctx;
}

void capture_panel::pre_frame() {
    clamp_drag_pre_frame();
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
            *ctx_->cur_sse = sse_state::connected;
            ctx_->log->add("INFO", "Connected to server");
        } else if (std::get_if<evt_disconnected>(&*ev)) {
            *ctx_->cur_sse   = sse_state::disconnected;
            *ctx_->capturing = false;
            ctx_->log->add("INFO", "Server disconnected");
        } else if (auto* e = std::get_if<evt_error>(&*ev)) {
            *ctx_->cur_sse = sse_state::error;
            ctx_->log->add("ERROR", ("Server error: " + e->message).c_str());
        } else if (auto* e = std::get_if<evt_capture_done>(&*ev)) {
            *ctx_->capturing = false;
            if (*ctx_->capture_mode == 0) {
                // Single mode: replace single viewer image
                ctx_->left_loader->start(e->path);
            } else if (*ctx_->capture_mode == 1) {
                // Compare mode: replace both
                ctx_->compare->unload_left();
                ctx_->compare->unload_right();
                ctx_->left_loader->start(e->path);
            } else {
                // Mode 2: keep left as reference, replace right only
                ctx_->compare->unload_right();
                ctx_->right_loader->start(e->path);
            }
            ctx_->log->add("INFO", ("Capture done: " + e->path).c_str());
        } else if (auto* e = std::get_if<evt_config_updated>(&*ev)) {
            *ctx_->cap_cfg  = e->cfg;
            *ctx_->conn_buf = make_conn_edit(*ctx_->cap_cfg);
            capture_config::save("visionstudio.json", *ctx_->cap_cfg);
            ctx_->log->add("INFO", "Config updated by server");
        } else if (auto* e = std::get_if<evt_camera_info>(&*ev)) {
            *ctx_->cam_info = std::move(e->groups);
        }
    }

    // Poll camera info future (Refresh button result)
    if (ctx_->cam_info_future->valid() &&
        ctx_->cam_info_future->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        *ctx_->cam_info = ctx_->cam_info_future->get();
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
    if (ctx_->cur_sse) *ctx_->cur_sse = sse_state::disconnected;
}

void capture_panel::render() {
    // Poll SSE events first.
    poll_events();

    if (!visible) return;

    // Get min_y from context and store for use in clamp_drag_pre_frame() next frame.
    const float min_y = ctx_->min_y;
    min_y_ = min_y;

    // First-frame placement when window doesn't exist yet.
    if (min_y > 0.0f && !ImGui::FindWindowByName("Capture Control##cap_panel"))
        ImGui::SetNextWindowPos({20.0f, min_y}, ImGuiCond_FirstUseEver);

    ImGui::SetNextWindowSize({220.0f, 600.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Capture Control##cap_panel", &visible)) { ImGui::End(); return; }

    // SSE status indicator
    const char* sse_label = "";
    ImVec4      sse_col   = {1, 1, 1, 1};
    switch (*ctx_->cur_sse) {
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
        ImGui::BeginDisabled(*ctx_->cur_sse == sse_state::connected);
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
            labeled("Host", [&]{ return ImGui::InputText("##host", ctx_->conn_buf->host, sizeof(ctx_->conn_buf->host)); });
            labeled("Port", [&]{ return ImGui::InputInt ("##port", &ctx_->conn_buf->port, 0); });
            ImGui::EndTable();
        }
        ImGui::Separator();
        if (ImGui::BeginTable("##conn_paths", 2, kTblFlags)) {
            ImGui::TableSetupColumn("##lbl2", ImGuiTableColumnFlags_WidthFixed, label_col_w);
            ImGui::TableSetupColumn("##val2", ImGuiTableColumnFlags_WidthStretch);
            labeled("Connect",    [&]{ return ImGui::InputText("##conn_path",  ctx_->conn_buf->connect_path,    sizeof(ctx_->conn_buf->connect_path)); });
            labeled("Start",      [&]{ return ImGui::InputText("##start_path", ctx_->conn_buf->start_path,      sizeof(ctx_->conn_buf->start_path)); });
            labeled("Stop",       [&]{ return ImGui::InputText("##stop_path",  ctx_->conn_buf->stop_path,       sizeof(ctx_->conn_buf->stop_path)); });
            labeled("Disconnect", [&]{ return ImGui::InputText("##disc_path",  ctx_->conn_buf->disconnect_path, sizeof(ctx_->conn_buf->disconnect_path)); });
            labeled("SSE",        [&]{ return ImGui::InputText("##sse_path",   ctx_->conn_buf->sse_path,        sizeof(ctx_->conn_buf->sse_path)); });
            labeled("Timeout(ms)",[&]{ return ImGui::InputInt ("##timeout",    &ctx_->conn_buf->timeout_ms,     0); });
            ImGui::EndTable();
        }
        if (conn_changed) {
            ctx_->cap_cfg->host            = ctx_->conn_buf->host;
            ctx_->cap_cfg->port            = ctx_->conn_buf->port;
            ctx_->cap_cfg->connect_path    = ctx_->conn_buf->connect_path;
            ctx_->cap_cfg->start_path      = ctx_->conn_buf->start_path;
            ctx_->cap_cfg->stop_path       = ctx_->conn_buf->stop_path;
            ctx_->cap_cfg->disconnect_path = ctx_->conn_buf->disconnect_path;
            ctx_->cap_cfg->sse_path        = ctx_->conn_buf->sse_path;
            ctx_->cap_cfg->timeout_ms      = ctx_->conn_buf->timeout_ms;
            capture_config::save("visionstudio.json", *ctx_->cap_cfg);
        }
        if (!ctx_->connect_cfg_tab->path.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Connect Config File");
            const auto& p   = ctx_->connect_cfg_tab->path;
            const auto  pos = p.find_last_of("/\\");
            const std::string fname = (pos == std::string::npos) ? p : p.substr(pos + 1);
            if (ImGui::Button(fname.c_str(), {-1, 0}))
                *ctx_->show_connect_config = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();

    // Connection buttons (blue)
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.15f, 0.45f, 0.75f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.22f, 0.58f, 0.90f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.10f, 0.32f, 0.55f, 1.0f});
    ImGui::BeginDisabled(*ctx_->cur_sse != sse_state::disconnected &&
                         *ctx_->cur_sse != sse_state::error);
    if (ImGui::Button("Connect", {-1, 0})) {
        cap_cli_.emplace(*ctx_->cap_cfg);
        cap_cli_->connect();
        *ctx_->cur_sse = sse_state::connecting;
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(*ctx_->cur_sse != sse_state::connected);
    if (ImGui::Button("Disconnect", {-1, 0})) {
        if (cap_cli_.has_value()) cap_cli_->disconnect();
        *ctx_->capturing = false;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::Separator();

    // Capture Settings (collapsible)
#ifndef NDEBUG
    constexpr bool cap_settings_disabled = false;
#else
    const bool cap_settings_disabled = *ctx_->cur_sse != sse_state::connected;
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
        const int prev_cap_mode = *ctx_->capture_mode;
        if (ImGui::Combo("##cap_mode", ctx_->capture_mode, kCaptureModes, 3)) {
            if (prev_cap_mode == 1 && *ctx_->capture_mode == 0) {
                ctx_->ref_img_path->clear();
                ctx_->compare->unload_left();
            }
            *ctx_->vmode = (*ctx_->capture_mode == 0) ? view_mode::single : view_mode::compare;
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
            sw_row("Image Acquisition", "##acq",  ctx_->image_acquisition);
            sw_row("Live Image",        "##live", ctx_->live_image);
            sw_row("Auto Detect",       "##ad",   ctx_->auto_detect);
            ImGui::EndTable();
        }

        ImGui::BeginDisabled(*ctx_->vmode != view_mode::compare);
        ImGui::TextDisabled("Ref Img");
        {
            const auto pos = ctx_->ref_img_path->find_last_of("/\\");
            const std::string fname = ctx_->ref_img_path->empty()
                ? "(none)"
                : (pos == std::string::npos ? *ctx_->ref_img_path : ctx_->ref_img_path->substr(pos + 1));
            if (ImGui::Button(fname.c_str(), {-1, 0})) {
                constexpr nfdfilteritem_t kTiffFilter[] = {{"TIFF Image", "tiff,tif"}};
                nfdchar_t* out = nullptr;
                if (NFD::OpenDialog(out, kTiffFilter, 1) == NFD_OKAY) {
                    *ctx_->ref_img_path = out;
                    NFD::FreePath(out);
                    ctx_->left_loader->start(*ctx_->ref_img_path);
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
                    capture_config::save("visionstudio.json", *ctx_->cap_cfg);
            };
            gl_row("basex",   "##basex",   &ctx_->cap_cfg->basex);
            gl_row("targetx", "##targetx", &ctx_->cap_cfg->targetx);
            gl_row("starty",  "##starty",  &ctx_->cap_cfg->starty);
            gl_row("liney",   "##liney",   &ctx_->cap_cfg->liney);
            ImGui::EndTable();
        }

        // Capture config files
        ImGui::Separator();
        ImGui::TextDisabled("Capture Config Files");
        if (!ctx_->capture_cfg_tab->path.empty()) {
            const auto& fpath = ctx_->capture_cfg_tab->path;
            const auto  pos   = fpath.find_last_of("/\\");
            const std::string fname = (pos == std::string::npos) ? fpath : fpath.substr(pos + 1);
            if (ImGui::Button(fname.c_str(), {-1, 0}))
                *ctx_->show_camera_config = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    // Capture buttons
    ImGui::BeginDisabled(*ctx_->cur_sse != sse_state::connected || *ctx_->capturing);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.18f, 0.55f, 0.18f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.25f, 0.70f, 0.25f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.12f, 0.40f, 0.12f, 1.0f});
    if (ImGui::Button("Start Capture", {-1, 0})) {
        if (cap_cli_.has_value()) {
            cap_cli_->start_capture();
            *ctx_->capturing = true;
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(*ctx_->cur_sse != sse_state::connected || !*ctx_->capturing);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.60f, 0.15f, 0.15f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.78f, 0.20f, 0.20f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.45f, 0.10f, 0.10f, 1.0f});
    if (ImGui::Button("Stop Capture", {-1, 0})) {
        if (cap_cli_.has_value()) {
            cap_cli_->stop_capture();
            *ctx_->capturing = false;
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    // Preview
    ImGui::Separator();
    const bool preview_on = is_preview_active();
    ImGui::BeginDisabled(preview_on);
    if (ImGui::Checkbox("Raw", &ctx_->cap_cfg->preview_raw))
        capture_config::save("visionstudio.json", *ctx_->cap_cfg);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(*ctx_->cur_sse != sse_state::connected);
    if (!preview_on) {
        if (ImGui::Button("Start Preview", {-1, 0})) {
            if (*ctx_->preview_tex != 0) {
                uint32_t tex = *ctx_->preview_tex;
                glDeleteTextures(1, &tex);
                *ctx_->preview_tex   = 0;
                *ctx_->preview_tex_w = 0;
                *ctx_->preview_tex_h = 0;
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
        const bool fetching = ctx_->cam_info_future->valid() &&
            ctx_->cam_info_future->wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        ImGui::BeginDisabled(fetching || *ctx_->cur_sse != sse_state::connected);
        if (ImGui::SmallButton(fetching ? "Fetching..." : "Refresh Info")) {
            if (cap_cli_.has_value()) {
                capture_client* cli = &*cap_cli_;
                *ctx_->cam_info_future = std::async(std::launch::async,
                    [cli]{ return cli->fetch_camera_info(); });
            }
        }
        ImGui::EndDisabled();

        for (auto& g : *ctx_->cam_info) {
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
