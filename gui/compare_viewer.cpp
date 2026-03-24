#include "gui/compare_viewer.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <thread>
#include <vector>

compare_viewer::compare_viewer() = default;

const image_data& compare_viewer::get_left_image_data()  const { return left_viewer_.get_image_data(); }
const image_data& compare_viewer::get_right_image_data() const { return right_viewer_.get_image_data(); }

void compare_viewer::set_left_overlays(std::vector<roi_entry> entries) {
    left_viewer_.set_overlays(std::move(entries));
}

void compare_viewer::set_right_overlays(std::vector<roi_entry> entries) {
    right_viewer_.set_overlays(std::move(entries));
}

void compare_viewer::set_split_overlays(std::vector<roi_entry> entries) {
    split_overlays_ = std::move(entries);
    apply_split_overlays();
}

void compare_viewer::clear_overlays() {
    split_overlays_.clear();
    left_viewer_.clear_overlays();
    right_viewer_.clear_overlays();
}

void compare_viewer::apply_split_overlays() {
    if (split_overlays_.empty() || split_src_.empty()) return;

    const int left_w = std::max(1, std::min(split_x, split_src_.width - 1));

    std::vector<roi_entry> left_ov, right_ov;
    for (const auto& e : split_overlays_) {
        const int x2 = e.x + e.w;

        // Left panel: clip to [0, left_w)
        if (e.x < left_w) {
            roi_entry le = e;
            le.w = std::min(x2, left_w) - e.x;
            if (le.w > 0) left_ov.push_back(le);
        }

        // Right panel: clip to [left_w, src_width), shift x by -left_w
        if (x2 > left_w) {
            roi_entry re = e;
            re.x = std::max(e.x, left_w) - left_w;
            re.w = x2 - std::max(e.x, left_w);
            if (re.w > 0) right_ov.push_back(re);
        }
    }

    left_viewer_.set_overlays(std::move(left_ov));
    right_viewer_.set_overlays(std::move(right_ov));
}

bool compare_viewer::load_left(const image_data& img) {
    is_split_     = false;
    diff_applied_ = false;
    return left_viewer_.load_image(img);
}

bool compare_viewer::load_right(const image_data& img) {
    is_split_     = false;
    right_orig_   = img;
    diff_applied_ = false;
    return right_viewer_.load_image(img);
}

bool compare_viewer::load_single(const image_data& img) {
    is_split_     = false;
    right_orig_   = img;
    diff_applied_ = false;
    const bool ok_l = left_viewer_.load_image(img);
    const bool ok_r = right_viewer_.load_image(img);
    return ok_l && ok_r;
}

bool compare_viewer::load_split(const image_data& img) {
    if (img.empty()) return false;
    split_src_      = img;
    is_split_       = true;
    split_x         = img.width / 2;  // default: midpoint
    split_x_applied_ = -1;            // force apply_split() on next render
    diff_applied_   = false;
    return true;
}

void compare_viewer::apply_split() {
    if (split_src_.empty()) return;

    const int left_w  = std::max(1, std::min(split_x, split_src_.width - 1));
    const int right_w = split_src_.width - left_w;
    const int h       = split_src_.height;

    image_data left_half, right_half;
    left_half.width   = left_w;
    left_half.height  = h;
    left_half.pixels.resize(static_cast<size_t>(left_w) * h * 4);
    right_half.width  = right_w;
    right_half.height = h;
    right_half.pixels.resize(static_cast<size_t>(right_w) * h * 4);

    for (int y = 0; y < h; ++y) {
        const auto row_src = split_src_.pixels.cbegin()
                           + static_cast<ptrdiff_t>(y) * split_src_.width * 4;
        std::copy(row_src,              row_src + left_w  * 4,
                  left_half.pixels.begin()  + static_cast<ptrdiff_t>(y) * left_w  * 4);
        std::copy(row_src + left_w * 4, row_src + (left_w + right_w) * 4,
                  right_half.pixels.begin() + static_cast<ptrdiff_t>(y) * right_w * 4);
    }

    right_orig_      = right_half;
    diff_applied_    = false;
    split_x_applied_ = split_x;
    left_viewer_.load_image(left_half);
    right_viewer_.load_image(right_half);
    apply_split_overlays();
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

    // Divide rows among threads; each thread writes to a disjoint output region.
    const int hw       = static_cast<int>(std::thread::hardware_concurrency());
    const int nthreads = std::max(1, std::min(hw, h));
    const float amplify = diff_amplify; // capture by value for thread safety

    auto compute_rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t dst = (static_cast<size_t>(y) * w       + x) * 4;
                const size_t li  = (static_cast<size_t>(y) * L.width + x) * 4;
                const size_t ri  = (static_cast<size_t>(y) * R.width + x) * 4;
                for (int c = 0; c < 3; ++c) {
                    const int diff      = static_cast<int>(L.pixels[li + c])
                                        - static_cast<int>(R.pixels[ri + c]);
                    const int amplified = static_cast<int>(std::abs(diff) * amplify);
                    diff_data_.pixels[dst + c] = static_cast<uint8_t>(std::min(255, amplified));
                }
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

    // Re-slice if split position changed.
    if (is_split_ && split_x != split_x_applied_) {
        apply_split();
    }

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

    left_viewer_.show_grid              = show_grid;
    left_viewer_.grid_spacing           = grid_spacing;
    left_viewer_.show_minimap           = show_minimap;
    left_viewer_.show_coordinates       = false;
    left_viewer_.show_overlays          = show_overlays;
    left_viewer_.show_crosshair         = show_crosshair;
    left_viewer_.minimap_force_aspect   = minimap_force_aspect;
    right_viewer_.show_grid             = show_grid;
    right_viewer_.grid_spacing          = grid_spacing;
    right_viewer_.show_minimap          = show_minimap;
    right_viewer_.show_coordinates      = false;
    right_viewer_.show_overlays         = show_overlays;
    right_viewer_.show_crosshair        = show_crosshair;
    right_viewer_.minimap_force_aspect  = minimap_force_aspect;

    view_state* left_state  = sync_views ? &shared_state_ : nullptr;
    view_state* right_state = sync_views ? &shared_state_ : nullptr;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList*  dl     = ImGui::GetWindowDrawList();

    // Labels
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
