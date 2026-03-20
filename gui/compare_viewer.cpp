#include "gui/compare_viewer.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>

compare_viewer::compare_viewer() = default;

bool compare_viewer::load_left(const image_data& img) {
    diff_applied_ = false; // force recompute on next render
    return left_viewer_.load_image(img);
}

bool compare_viewer::load_right(const image_data& img) {
    right_orig_   = img;   // keep original for diff toggle
    diff_applied_ = false;
    return right_viewer_.load_image(img);
}

bool compare_viewer::load_single(const image_data& img) {
    right_orig_   = img;   // keep copy as original right for diff toggle
    diff_applied_ = false;
    const bool ok_l = left_viewer_.load_image(img);
    const bool ok_r = right_viewer_.load_image(img);
    return ok_l && ok_r;
}

// ---------------------------------------------------------------------------
// Diff helpers
// ---------------------------------------------------------------------------

void compare_viewer::compute_diff() {
    const image_data& L = left_viewer_.get_image_data();
    const image_data& R = right_orig_;
    if (L.empty() || R.empty()) return;

    const int w = std::min(L.width,  R.width);
    const int h = std::min(L.height, R.height);
    diff_data_.width  = w;
    diff_data_.height = h;
    diff_data_.pixels.resize(static_cast<size_t>(w) * h * 4);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) { // R, G, B
                const int diff = static_cast<int>(L.pixels[idx + c])
                               - static_cast<int>(R.pixels[idx + c]);
                const int amplified = static_cast<int>(std::abs(diff) * diff_amplify);
                diff_data_.pixels[idx + c] = static_cast<uint8_t>(std::min(255, amplified));
            }
            diff_data_.pixels[idx + 3] = 255; // A always opaque
        }
    }
}

void compare_viewer::update_right_viewer() {
    if (diff_mode) {
        compute_diff();
        if (!diff_data_.empty())
            right_viewer_.load_image(diff_data_);
    } else {
        if (!right_orig_.empty())
            right_viewer_.load_image(right_orig_);
    }
    diff_applied_ = diff_mode;
}

// ---------------------------------------------------------------------------
// Label helper
// ---------------------------------------------------------------------------

static std::string label_text(const std::string& path, const char* fallback) {
    if (path.empty()) return fallback;
    const auto pos = path.rfind('/');
    const std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);
    const std::string dir  = (pos == std::string::npos) ? "" : path.substr(0, pos);
    return dir.empty() ? name : name + "  (" + dir + ")";
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void compare_viewer::render(float width, float height) {
    if (width  <= 0.0f) width  = ImGui::GetContentRegionAvail().x;
    if (height <= 0.0f) height = ImGui::GetContentRegionAvail().y;
    if (width  < 2.0f)  width  = 2.0f;
    if (height < 1.0f)  height = 1.0f;

    // Reload right viewer if diff_mode or amplify changed.
    if (diff_mode != diff_applied_ ||
        (diff_mode && diff_amplify != amplify_applied_)) {
        update_right_viewer();
        amplify_applied_ = diff_amplify;
    }

    const float spacing  = ImGui::GetStyle().ItemSpacing.x;
    const float half_w   = std::floor((width - spacing) * 0.5f);
    const float label_h  = ImGui::GetTextLineHeightWithSpacing();
    const float canvas_h = height - label_h;

    left_viewer_.show_grid         = show_grid;
    left_viewer_.grid_spacing      = grid_spacing;
    left_viewer_.show_minimap      = show_minimap;
    left_viewer_.show_coordinates  = false;
    right_viewer_.show_grid        = show_grid;
    right_viewer_.grid_spacing     = grid_spacing;
    right_viewer_.show_minimap     = show_minimap;
    right_viewer_.show_coordinates = false;

    view_state* left_state  = sync_views ? &shared_state_ : nullptr;
    view_state* right_state = sync_views ? &shared_state_ : nullptr;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList*  dl     = ImGui::GetWindowDrawList();

    // Labels
    const ImU32 label_col = IM_COL32(180, 180, 180, 255);
    const std::string ltxt = label_text(left_label, "Left");
    const std::string rtxt = diff_mode
        ? label_text(right_label, "Right") + "  [Diff]"
        : label_text(right_label, "Right");

    dl->PushClipRect(origin, {origin.x + half_w, origin.y + label_h}, true);
    dl->AddText({origin.x + 4.0f, origin.y + 2.0f}, label_col, ltxt.c_str());
    dl->PopClipRect();

    dl->PushClipRect({origin.x + half_w + spacing, origin.y},
                     {origin.x + half_w + spacing + half_w, origin.y + label_h}, true);
    // Highlight the diff label in orange
    const ImU32 rtxt_col = diff_mode ? IM_COL32(255, 180, 60, 255) : label_col;
    dl->AddText({origin.x + half_w + spacing + 4.0f, origin.y + 2.0f}, rtxt_col, rtxt.c_str());
    dl->PopClipRect();

    // Canvases
    ImGui::SetCursorScreenPos({origin.x, origin.y + label_h});
    left_viewer_.render("left_canvas", half_w, canvas_h, left_state);

    ImGui::SetCursorScreenPos({origin.x + half_w + spacing, origin.y + label_h});
    right_viewer_.render("right_canvas", half_w, canvas_h, right_state);

    ImGui::SetCursorScreenPos({origin.x, origin.y + height});

    // ----- Combined pixel tooltip -----
    if (!show_coordinates) return;

    const ImVec2 canvas_size      = {half_w, canvas_h};
    const ImVec2 left_canvas_pos  = {origin.x,                    origin.y + label_h};
    const ImVec2 right_canvas_pos = {origin.x + half_w + spacing, origin.y + label_h};

    const view_state& lstateref = sync_views ? shared_state_ : left_viewer_.get_view_state();
    const view_state& rstateref = sync_views ? shared_state_ : right_viewer_.get_view_state();

    const auto lq = left_viewer_.query_mouse_pixel(left_canvas_pos,  canvas_size, lstateref);
    const auto rq = right_viewer_.query_mouse_pixel(right_canvas_pos, canvas_size, rstateref);

    int ref_x = 0, ref_y = 0;
    bool show = false;
    if      (lq.valid) { ref_x = lq.img_x; ref_y = lq.img_y; show = true; }
    else if (rq.valid) { ref_x = rq.img_x; ref_y = rq.img_y; show = true; }
    if (!show) return;

    const auto left_rgba  = lq.valid ? lq.rgba : left_viewer_.pixel_at(ref_x, ref_y);
    const auto right_rgba = rq.valid ? rq.rgba : right_viewer_.pixel_at(ref_x, ref_y);

    auto color_vec = [](const std::array<uint8_t, 4>& c) {
        return ImVec4(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f);
    };

    ImGui::BeginTooltip();
    ImGui::Text("pos  : (%d, %d)", ref_x, ref_y);
    ImGui::Text("zoom : %.2fx", static_cast<double>(lstateref.zoom));
    ImGui::Separator();
    ImGui::TextDisabled("Left");
    ImGui::ColorButton("##lswatch", color_vec(left_rgba),
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, {16, 16});
    ImGui::SameLine();
    ImGui::Text("R:%3d  G:%3d  B:%3d  A:%3d",
                left_rgba[0], left_rgba[1], left_rgba[2], left_rgba[3]);
    ImGui::Spacing();
    ImGui::TextDisabled(diff_mode ? "Diff" : "Right");
    ImGui::ColorButton("##rswatch", color_vec(right_rgba),
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, {16, 16});
    ImGui::SameLine();
    ImGui::Text("R:%3d  G:%3d  B:%3d  A:%3d",
                right_rgba[0], right_rgba[1], right_rgba[2], right_rgba[3]);
    ImGui::EndTooltip();
}
