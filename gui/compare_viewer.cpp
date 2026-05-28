#include "gui/compare_viewer.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <thread>
#include <vector>
#include <chrono>

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
    cancel_diff();
    left_src_             = img;
    left_offset_x         = 0;
    left_offset_y         = 0;
    left_offset_applied_x = 0;
    left_offset_applied_y = 0;
    diff_applied_         = false;
    left_viewer_.set_display_offset(0, 0);
    return left_viewer_.load_image(img);
}

void compare_viewer::unload_left() {
    left_src_ = {};
    diff_applied_ = false;
    left_viewer_.set_display_offset(0, 0);
    left_viewer_.unload_image();
}

bool compare_viewer::load_right(const image_data& img) {
    cancel_diff();
    right_src_             = img;
    right_orig_            = img;
    right_offset_x         = 0;
    right_offset_y         = 0;
    right_offset_applied_x = 0;
    right_offset_applied_y = 0;
    diff_applied_          = false;
    right_viewer_.set_display_offset(0, 0);
    return right_viewer_.load_image(img);
}

bool compare_viewer::load_single(const image_data& img) {
    cancel_diff();
    left_src_              = img;
    left_offset_x          = 0;
    left_offset_y          = 0;
    left_offset_applied_x  = 0;
    left_offset_applied_y  = 0;
    right_src_             = img;
    right_orig_            = img;
    right_offset_x         = 0;
    right_offset_y         = 0;
    right_offset_applied_x = 0;
    right_offset_applied_y = 0;
    diff_applied_          = false;
    left_viewer_.set_display_offset(0, 0);
    right_viewer_.set_display_offset(0, 0);
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

    const int  lch     = L.channels();
    const int  rch     = R.channels();
    const bool is_gray = (lch == 1 && rch == 1);

    // Precompute LUT: abs_diff [0,255] -> amplified+clamped byte.
    // Avoids a float multiply and branch per pixel.
    uint8_t lut[256];
    const float amp = diff_amplify;
    for (int i = 0; i < 256; ++i)
        lut[i] = static_cast<uint8_t>(std::min(255, static_cast<int>(i * amp)));

    const int hw       = static_cast<int>(std::thread::hardware_concurrency());
    const int nthreads = std::max(1, std::min(hw, h));

    auto launch = [&](auto compute_rows) {
        if (nthreads == 1) { compute_rows(0, h); return; }
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (int t = 0; t < nthreads; ++t)
            threads.emplace_back(compute_rows, t * h / nthreads, (t + 1) * h / nthreads);
        for (auto& th : threads) th.join();
    };

    if (is_gray) {
        // Gray fast path: 1 byte/pixel output (4x less data than RGBA).
        diff_data_.format = PixelFormat::gray;
        diff_data_.pixels.resize(static_cast<size_t>(w) * h);

        launch([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                if (diff_cancel_.load(std::memory_order_relaxed)) return;
                const uint8_t* lp = L.pixels.data() + static_cast<size_t>(y) * L.width;
                const uint8_t* rp = R.pixels.data() + static_cast<size_t>(y) * R.width;
                uint8_t*       dp = diff_data_.pixels.data() + static_cast<size_t>(y) * w;
                for (int x = 0; x < w; ++x) {
                    const int d = static_cast<int>(lp[x]) - static_cast<int>(rp[x]);
                    dp[x] = lut[d < 0 ? -d : d];
                }
                diff_rows_done_.fetch_add(1, std::memory_order_relaxed);
            }
        });
    } else {
        diff_data_.format = PixelFormat::rgba;
        diff_data_.pixels.resize(static_cast<size_t>(w) * h * 4);
        const int nch = std::min({3, lch, rch});

        launch([&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                if (diff_cancel_.load(std::memory_order_relaxed)) return;
                const uint8_t* lp = L.pixels.data() + static_cast<size_t>(y) * L.width * lch;
                const uint8_t* rp = R.pixels.data() + static_cast<size_t>(y) * R.width * rch;
                uint8_t*       dp = diff_data_.pixels.data() + static_cast<size_t>(y) * w * 4;
                for (int x = 0; x < w; ++x, lp += lch, rp += rch, dp += 4) {
                    for (int c = 0; c < nch; ++c) {
                        const int d = static_cast<int>(lp[c]) - static_cast<int>(rp[c]);
                        dp[c] = lut[d < 0 ? -d : d];
                    }
                    for (int c = nch; c < 3; ++c) dp[c] = dp[0];
                    dp[3] = 255;
                }
                diff_rows_done_.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
}

void compare_viewer::apply_left_offset() {
    if (left_src_.empty()) return;
    const int ox = std::max(0, std::min(left_offset_x, left_src_.width  - 1));
    const int oy = std::max(0, std::min(left_offset_y, left_src_.height - 1));
    left_offset_applied_x = ox;
    left_offset_applied_y = oy;

    if (ox == 0 && oy == 0) {
        left_viewer_.set_display_offset(ox, oy);
    left_viewer_.load_image(left_src_, false);
    } else {
        image_data sliced;
        sliced.width  = left_src_.width  - ox;
        sliced.height = left_src_.height - oy;
        sliced.format = left_src_.format;
        const int ch = left_src_.channels();
        sliced.pixels.resize(static_cast<size_t>(sliced.width) * sliced.height * ch);
        for (int y = 0; y < sliced.height; ++y) {
            const uint8_t* src = left_src_.pixels.data()
                               + (static_cast<size_t>(oy + y) * left_src_.width + ox) * ch;
            uint8_t* dst = sliced.pixels.data()
                         + static_cast<size_t>(y) * sliced.width * ch;
            std::memcpy(dst, src, static_cast<size_t>(sliced.width) * ch);
        }
        left_viewer_.set_display_offset(ox, oy);
        left_viewer_.load_image(sliced, false);
    }
    diff_applied_ = false;
}

void compare_viewer::apply_right_offset() {
    if (right_src_.empty()) return;
    const int ox = std::max(0, std::min(right_offset_x, right_src_.width  - 1));
    const int oy = std::max(0, std::min(right_offset_y, right_src_.height - 1));
    right_offset_applied_x = ox;
    right_offset_applied_y = oy;

    if (ox == 0 && oy == 0) {
        right_orig_ = right_src_;
    } else {
        image_data sliced;
        sliced.width  = right_src_.width  - ox;
        sliced.height = right_src_.height - oy;
        sliced.format = right_src_.format;
        const int ch = right_src_.channels();
        sliced.pixels.resize(static_cast<size_t>(sliced.width) * sliced.height * ch);
        for (int y = 0; y < sliced.height; ++y) {
            const uint8_t* src = right_src_.pixels.data()
                               + (static_cast<size_t>(oy + y) * right_src_.width + ox) * ch;
            uint8_t* dst = sliced.pixels.data()
                         + static_cast<size_t>(y) * sliced.width * ch;
            std::memcpy(dst, src, static_cast<size_t>(sliced.width) * ch);
        }
        right_orig_ = std::move(sliced);
    }
    right_viewer_.set_display_offset(ox, oy);
    // In diff mode the viewer is reloaded by update_right_viewer() after
    // the async diff finishes — loading the original here would cause a flicker.
    if (!diff_mode)
        right_viewer_.load_image(right_orig_, false);
    diff_applied_ = false;
}

void compare_viewer::cancel_diff() {
    if (diff_future_.valid()) {
        diff_cancel_.store(true, std::memory_order_relaxed);
        diff_future_.wait();
        diff_cancel_.store(false, std::memory_order_relaxed);
    }
}

void compare_viewer::update_right_viewer() {
    if (diff_mode) {
        cancel_diff();
        const int h = std::min(left_viewer_.get_image_data().height,
                               right_orig_.empty() ? 0 : right_orig_.height);
        diff_total_rows_ = h;
        diff_rows_done_.store(0, std::memory_order_relaxed);
        diff_future_ = std::async(std::launch::async,
                                  [this]{ compute_diff(); });
    } else {
        cancel_diff();
        if (!right_orig_.empty())
            right_viewer_.load_image(right_orig_);
        diff_applied_ = false;
    }
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

    // Offset is applied after the DragInt controls (below), not here, so that
    // re-slicing is skipped while dragging and only fires on release.

    // Poll async diff completion — load result on the main thread (OpenGL).
    const bool diff_computing = diff_future_.valid() &&
        diff_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (!diff_computing && diff_future_.valid()) {
        diff_future_.get();
        if (!diff_data_.empty())
            right_viewer_.load_image(diff_data_);
        diff_applied_    = diff_mode;
        amplify_applied_ = diff_amplify;
    }

    // Trigger async diff when mode or amplify changes (only when no compute in flight).
    if (!diff_computing &&
        (diff_mode != diff_applied_ ||
         (diff_mode && diff_amplify != amplify_applied_))) {
        update_right_viewer();
    }

    const float spacing    = ImGui::GetStyle().ItemSpacing.x;
    const float half_w     = std::floor((width - spacing) * 0.5f);
    const float label_h    = ImGui::GetTextLineHeightWithSpacing();
    const bool  has_left_offset  = !left_src_.empty();
    const bool  has_right_offset = !right_src_.empty();
    const bool  has_offset       = has_left_offset || has_right_offset;
    const float offset_h         = has_offset ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
    const float canvas_h   = height - label_h - offset_h;

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

    // Advance ImGui's cursor to the offset-controls row. The label row above was
    // drawn via dl->AddText which does not advance the cursor, so without this
    // dummy item ImGui's layout state is uninitialised when the LEFT controls call
    // SetCursorScreenPos as the first ImGui item — causing them to render at the
    // wrong Y. The Dummy renders zero pixels but initialises DC.PrevLineSize.y.
    if (has_offset) {
        ImGui::SetCursorScreenPos({origin.x, origin.y + label_h});
        ImGui::Dummy({0.0f, 0.0f});
    }

    // ----- Offset controls (above each canvas) -----
    // Returns true when the offset value was committed (drag released / enter / reset).
    // DragInt width: fit two drags + two "X:" labels + Reset button within half_w.
    const float kLabelW  = ImGui::CalcTextSize("X:").x + 4.0f;
    const float kResetW  = ImGui::CalcTextSize("Reset").x
                         + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float kSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float drag_w   = std::max(60.0f,
        (half_w - kLabelW * 2.0f - kResetW - kSpacing * 4.0f) * 0.5f);

    auto draw_offset_controls = [&](float panel_x, int& ox, int& oy,
                                    int max_x, int max_y,
                                    const char* id_x, const char* id_y,
                                    const char* id_reset) -> bool {
        bool apply = false;
        ImGui::SetCursorScreenPos({panel_x, origin.y + label_h});
        ImGui::TextDisabled("X:");
        ImGui::SameLine(0, 2.0f);
        ImGui::SetNextItemWidth(drag_w);
        ImGui::DragInt(id_x, &ox, 1.0f, 0, max_x, "%d");
        if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;
        ImGui::SameLine();
        ImGui::TextDisabled("Y:");
        ImGui::SameLine(0, 2.0f);
        ImGui::SetNextItemWidth(drag_w);
        ImGui::DragInt(id_y, &oy, 1.0f, 0, max_y, "%d");
        if (ImGui::IsItemDeactivatedAfterEdit()) apply = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(ox == 0 && oy == 0);
        if (ImGui::SmallButton(id_reset)) { ox = 0; oy = 0; apply = true; }
        ImGui::EndDisabled();
        return apply;
    };

    if (has_left_offset &&
            draw_offset_controls(origin.x, left_offset_x, left_offset_y,
                                 left_src_.width - 1, left_src_.height - 1,
                                 "##lox", "##loy", "Reset##loff")) {
        apply_left_offset();
        if (diff_mode) update_right_viewer();
    }

    if (has_right_offset &&
            draw_offset_controls(origin.x + half_w + spacing,
                                 right_offset_x, right_offset_y,
                                 right_src_.width - 1, right_src_.height - 1,
                                 "##rox", "##roy", "Reset##roff")) {
        apply_right_offset();
        if (diff_mode) update_right_viewer();
    }

    const float canvas_top = origin.y + label_h + offset_h;

    ImGui::SetCursorScreenPos({origin.x, canvas_top});
    {
        const auto& rh = right_viewer_.get_hover_info();
        left_viewer_.set_peer_crosshair(rh.img_x, rh.img_y,
                                        show_crosshair && rh.valid);
    }
    left_viewer_.render("left_canvas", half_w, canvas_h, left_state);

    ImGui::SetCursorScreenPos({origin.x + half_w + spacing, canvas_top});
    {
        const auto& lh = left_viewer_.get_hover_info();
        right_viewer_.set_peer_crosshair(lh.img_x, lh.img_y,
                                         show_crosshair && lh.valid);
    }
    right_viewer_.render("right_canvas", half_w, canvas_h, right_state);

    // Draw filename labels after viewers so they appear on top of the image.
    dl->PushClipRect(origin, {origin.x + half_w, origin.y + label_h}, true);
    dl->AddText({origin.x + 4.0f, origin.y + 2.0f}, label_col, ltxt.c_str());
    dl->PopClipRect();
    dl->PushClipRect({origin.x + half_w + spacing, origin.y},
                     {origin.x + half_w + spacing + half_w, origin.y + label_h}, true);
    const ImU32 rtxt_col = diff_mode ? IM_COL32(255, 180, 60, 255) : label_col;
    dl->AddText({origin.x + half_w + spacing + 4.0f, origin.y + 2.0f}, rtxt_col, rtxt.c_str());
    dl->PopClipRect();

    // Progress overlay while diff is computing.
    if (diff_computing) {
        const float progress = diff_total_rows_ > 0
            ? std::min(1.0f, static_cast<float>(diff_rows_done_.load(std::memory_order_relaxed))
                             / diff_total_rows_)
            : 0.0f;
        const ImVec2 rp  = {origin.x + half_w + spacing, canvas_top};
        const ImVec2 rp2 = {rp.x + half_w, rp.y + canvas_h};
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(rp, rp2, IM_COL32(0, 0, 0, 110));
        constexpr float bar_h  = 18.0f;
        constexpr float bar_pw = 0.55f;
        const float bar_w  = half_w * bar_pw;
        const float bar_x  = rp.x + (half_w - bar_w) * 0.5f;
        const float bar_y  = rp.y + (canvas_h - bar_h) * 0.5f;
        ImGui::SetCursorScreenPos({bar_x, bar_y});
        ImGui::ProgressBar(progress, {bar_w, bar_h}, "Computing diff...");
    }

    ImGui::SetCursorScreenPos({origin.x, origin.y + height});

    // ----- Combined pixel info (tooltip + pixel panel) -----
    const ImVec2 canvas_size      = {half_w, canvas_h};
    const ImVec2 left_canvas_pos  = {origin.x,                    canvas_top};
    const ImVec2 right_canvas_pos = {origin.x + half_w + spacing, canvas_top};

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
