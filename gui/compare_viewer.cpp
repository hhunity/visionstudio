#include "gui/compare_viewer.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <thread>
#include <vector>

compare_viewer::compare_viewer() = default;

const image_data& compare_viewer::get_left_image_data()  const { return left_viewer_.get_image_data(); }
const image_data& compare_viewer::get_right_image_data() const { return right_viewer_.get_image_data(); }

void compare_viewer::set_left_overlay_groups(std::vector<roi_group> groups) {
    left_viewer_.set_overlay_groups(std::move(groups));
}

void compare_viewer::set_right_overlay_groups(std::vector<roi_group> groups) {
    right_viewer_.set_overlay_groups(std::move(groups));
}

void compare_viewer::clear_overlays() {
    left_viewer_.clear_overlays();
    right_viewer_.clear_overlays();
}

bool compare_viewer::load_left(const image_data& img) {
    diff_applied_ = false;
    return left_viewer_.load_image(img);
}

void compare_viewer::unload_left() {
    diff_applied_ = false;
    left_viewer_.unload_image();
}

bool compare_viewer::load_right(const image_data& img) {
    right_orig_   = img;
    diff_applied_ = false;
    return right_viewer_.load_image(img);
}

bool compare_viewer::load_single(const image_data& img) {
    right_orig_   = img;
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
    diff_data_.format = PixelFormat::rgba;
    diff_data_.pixels.resize(static_cast<size_t>(w) * h * 4);

    const int lch = L.channels();
    const int rch = R.channels();

    // Divide rows among threads; each thread writes to a disjoint output region.
    const int hw       = static_cast<int>(std::thread::hardware_concurrency());
    const int nthreads = std::max(1, std::min(hw, h));
    const float amplify = diff_amplify;

    auto compute_rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t dst = (static_cast<size_t>(y) * w       + x) * 4;
                const size_t li  = (static_cast<size_t>(y) * L.width + x) * lch;
                const size_t ri  = (static_cast<size_t>(y) * R.width + x) * rch;
                // Compare up to 3 channels; for gray images only channel 0 is used.
                const int nch = std::min({3, lch, rch});
                for (int c = 0; c < nch; ++c) {
                    const int diff      = static_cast<int>(L.pixels[li + c])
                                        - static_cast<int>(R.pixels[ri + c]);
                    const int amplified = static_cast<int>(std::abs(diff) * amplify);
                    diff_data_.pixels[dst + c] = static_cast<uint8_t>(std::min(255, amplified));
                }
                // Fill remaining RGB channels with first-channel diff (gray→gray case)
                for (int c = nch; c < 3; ++c)
                    diff_data_.pixels[dst + c] = diff_data_.pixels[dst];
                diff_data_.pixels[dst + 3] = 255;
            }
        }
    };

    if (nthreads == 1) {
        compute_rows(0, h);
        return;
    }

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) {
        const int y0 = t       * h / nthreads;
        const int y1 = (t + 1) * h / nthreads;
        threads.emplace_back(compute_rows, y0, y1);
    }
    for (auto& th : threads) th.join();
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

    left_viewer_.show_grid             = show_grid;
    left_viewer_.grid_spacing          = grid_spacing;
    left_viewer_.show_minimap          = show_minimap;
    left_viewer_.show_coordinates      = false;
    left_viewer_.show_overlays         = show_left_overlays;
    left_viewer_.show_crosshair        = show_crosshair;
    left_viewer_.minimap_force_aspect  = minimap_force_aspect;
    right_viewer_.show_grid            = show_grid;
    right_viewer_.grid_spacing         = grid_spacing;
    right_viewer_.show_minimap         = show_minimap;
    right_viewer_.show_coordinates     = false;
    right_viewer_.show_overlays        = show_right_overlays;
    right_viewer_.show_crosshair       = show_crosshair;
    right_viewer_.minimap_force_aspect = minimap_force_aspect;

    view_state* left_state  = sync_views ? &shared_state_ : nullptr;
    view_state* right_state = sync_views ? &shared_state_ : nullptr;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList*  dl     = ImGui::GetWindowDrawList();

    const ImU32 label_col = IM_COL32(180, 180, 180, 255);

    auto with_size = [](std::string label, const image_data& img) {
        if (img.empty()) return label;
        return label + "  " + std::to_string(img.width) + " x " + std::to_string(img.height);
    };

    const std::string ltxt = with_size(label_text(left_label, "Left"),
                                       left_viewer_.get_image_data());
    const std::string rtxt = with_size(
        diff_mode ? label_text(right_label, "Right") + "  [Diff]"
                  : label_text(right_label, "Right"),
        right_viewer_.get_image_data());

    dl->PushClipRect(origin, {origin.x + half_w, origin.y + label_h}, true);
    dl->AddText({origin.x + 4.0f, origin.y + 2.0f}, label_col, ltxt.c_str());
    dl->PopClipRect();

    dl->PushClipRect({origin.x + half_w + spacing, origin.y},
                     {origin.x + half_w + spacing + half_w, origin.y + label_h}, true);
    const ImU32 rtxt_col = diff_mode ? IM_COL32(255, 180, 60, 255) : label_col;
    dl->AddText({origin.x + half_w + spacing + 4.0f, origin.y + 2.0f}, rtxt_col, rtxt.c_str());
    dl->PopClipRect();

    ImGui::SetCursorScreenPos({origin.x, origin.y + label_h});
    left_viewer_.render("left_canvas", half_w, canvas_h, left_state);

    ImGui::SetCursorScreenPos({origin.x + half_w + spacing, origin.y + label_h});
    right_viewer_.render("right_canvas", half_w, canvas_h, right_state);

    ImGui::SetCursorScreenPos({origin.x, origin.y + height});

    // ----- Combined pixel info (tooltip + pixel panel) -----
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

    combined_hover_.valid = show;
    if (show) {
        combined_hover_.img_x      = ref_x;
        combined_hover_.img_y      = ref_y;
        combined_hover_.zoom       = lstateref.zoom;
        combined_hover_.left_rgba  = lq.valid ? lq.rgba : left_viewer_.pixel_at(ref_x, ref_y);
        combined_hover_.right_rgba = rq.valid ? rq.rgba : right_viewer_.pixel_at(ref_x, ref_y);
    }

    if (!show_coordinates || !show) return;

    auto color_vec = [](const std::array<uint8_t, 4>& c) {
        return ImVec4(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f);
    };

    ImGui::BeginTooltip();
    ImGui::Text("pos  : (%d, %d)", ref_x, ref_y);
    ImGui::Text("zoom : %.2fx", static_cast<double>(lstateref.zoom));
    ImGui::Separator();
    ImGui::TextDisabled("Left");
    ImGui::ColorButton("##lswatch", color_vec(combined_hover_.left_rgba),
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, {16, 16});
    ImGui::SameLine();
    ImGui::Text("R:%3d  G:%3d  B:%3d  A:%3d",
                combined_hover_.left_rgba[0], combined_hover_.left_rgba[1],
                combined_hover_.left_rgba[2], combined_hover_.left_rgba[3]);
    ImGui::Spacing();
    ImGui::TextDisabled(diff_mode ? "Diff" : "Right");
    ImGui::ColorButton("##rswatch", color_vec(combined_hover_.right_rgba),
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, {16, 16});
    ImGui::SameLine();
    ImGui::Text("R:%3d  G:%3d  B:%3d  A:%3d",
                combined_hover_.right_rgba[0], combined_hover_.right_rgba[1],
                combined_hover_.right_rgba[2], combined_hover_.right_rgba[3]);
    ImGui::EndTooltip();
}
