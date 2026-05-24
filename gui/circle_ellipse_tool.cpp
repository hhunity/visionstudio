#include "gui/circle_ellipse_tool.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <imgui.h>
#include <implot.h>
#include <nfd.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <string>

// ---------------------------------------------------------------------------
// Image data → grayscale OpenCV Mat
// ---------------------------------------------------------------------------

static cv::Mat to_gray_mat(const image_data& img) {
    if (img.channels() == 1) {
        return cv::Mat(img.height, img.width, CV_8UC1,
                       const_cast<uint8_t*>(img.pixels.data())).clone();
    }
    cv::Mat rgba(img.height, img.width, CV_8UC4,
                 const_cast<uint8_t*>(img.pixels.data()));
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    return gray;
}

// ---------------------------------------------------------------------------
// Core detection logic (static — safe to call from background thread)
// ---------------------------------------------------------------------------

// Normalized mean distance between contour points and the fitted ellipse.
// Returns 0 for a perfect fit, larger values for poor fits.
float circle_ellipse_tool::ellipse_fit_error(
    const std::vector<cv::Point>& contour, const cv::RotatedRect& ell)
{
    const float a   = ell.size.width  * 0.5f;
    const float b   = ell.size.height * 0.5f;
    if (a <= 0.0f || b <= 0.0f) return 1e9f;
    const float rad = ell.angle * (static_cast<float>(std::numbers::pi) / 180.0f);
    const float ca  = std::cos(rad);
    const float sa  = std::sin(rad);
    float err = 0.0f;
    for (const auto& pt : contour) {
        const float dx =  static_cast<float>(pt.x) - ell.center.x;
        const float dy =  static_cast<float>(pt.y) - ell.center.y;
        const float rx =  dx * ca + dy * sa;
        const float ry = -dx * sa + dy * ca;
        // Distance from the unit ellipse surface (|value - 1| where value == 1 on the ellipse)
        err += std::abs(std::sqrt((rx / a) * (rx / a) + (ry / b) * (ry / b)) - 1.0f);
    }
    return err / static_cast<float>(contour.size());
}

std::vector<detected_shape> circle_ellipse_tool::run_detection(
    const image_data& img,
    float canny_t1, float canny_t2,
    float min_radius, float max_radius,
    float hough_dp, float hough_min_dist, float hough_param2,
    float min_axis_ratio, int min_contour_px,
    bool detect_circles, bool detect_ellipses,
    int max_detect_size, float max_fit_error,
    std::atomic<float>* progress,
    std::atomic<bool>*  cancel)
{
    auto set_progress  = [&](float v) { if (progress) progress->store(v); };
    auto is_cancelled  = [&]{ return cancel && cancel->load(); };

    std::vector<detected_shape> shapes;
    if (img.empty()) return shapes;

    set_progress(0.0f);
    if (is_cancelled()) return {};

    cv::Mat gray = to_gray_mat(img);

    // Downsample to max_detect_size on the longest side.
    // Detected coords/radii are scaled back to original image space.
    const float scale = (max_detect_size > 0)
        ? std::min(1.0f, static_cast<float>(max_detect_size)
                         / static_cast<float>(std::max(img.width, img.height)))
        : 1.0f;
    cv::Mat work;
    if (scale < 1.0f)
        cv::resize(gray, work, {}, scale, scale, cv::INTER_AREA);
    else
        work = gray;

    cv::Mat blurred;
    cv::GaussianBlur(work, blurred, {5, 5}, 1.5);
    set_progress(0.1f);

    // Scaled params for the downsampled image
    const float s_min_r    = min_radius    * scale;
    const float s_max_r    = max_radius    * scale;
    const float s_min_dist = hough_min_dist * scale;
    const float s_min_area = static_cast<float>(min_contour_px) * scale * scale;

    if (is_cancelled()) return {};

    // ----- HoughCircles -----
    if (detect_circles) {
        std::vector<cv::Vec3f> circles;
        cv::HoughCircles(blurred, circles, cv::HOUGH_GRADIENT,
                         static_cast<double>(hough_dp),
                         static_cast<double>(s_min_dist),
                         static_cast<double>(canny_t2),
                         static_cast<double>(hough_param2),
                         static_cast<int>(s_min_r),
                         static_cast<int>(s_max_r));
        for (const auto& c : circles) {
            detected_shape sh;
            sh.kind      = detected_shape::kind_t::circle;
            sh.cx        = c[0] / scale;   // 元スケールに戻す
            sh.cy        = c[1] / scale;
            sh.rx        = c[2] / scale;
            sh.ry        = c[2] / scale;
            sh.angle_deg = 0.0f;
            shapes.push_back(sh);
        }
    }
    set_progress(0.4f);

    if (is_cancelled()) return {};

    // ----- Contour-based ellipse fitting -----
    if (detect_ellipses) {
        cv::Mat edges;
        cv::Canny(blurred, edges, canny_t1, canny_t2);
        set_progress(0.55f);

        if (is_cancelled()) return {};

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_NONE);

        const float n = static_cast<float>(contours.size());
        for (size_t ci = 0; ci < contours.size(); ++ci) {
            if (is_cancelled()) return {};
            set_progress(0.55f + 0.45f * (static_cast<float>(ci) / std::max(n, 1.0f)));

            const auto& contour = contours[ci];
            if (contour.size() < 5) continue;
            if (cv::contourArea(contour) < s_min_area) continue;

            const cv::RotatedRect ell = cv::fitEllipse(contour);
            const float rx = ell.size.width  * 0.5f;
            const float ry = ell.size.height * 0.5f;
            if (rx <= 0.0f || ry <= 0.0f) continue;

            const float major = std::max(rx, ry);
            const float minor = std::min(rx, ry);
            if (major < s_min_r || major > s_max_r) continue;
            if (minor / major < min_axis_ratio) continue;
            if (ellipse_fit_error(contour, ell) > max_fit_error) continue;

            bool dup = false;
            for (const auto& existing : shapes) {
                const float dx = ell.center.x / scale - existing.cx;
                const float dy = ell.center.y / scale - existing.cy;
                if (std::sqrt(dx * dx + dy * dy) < hough_min_dist * 0.5f) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            detected_shape sh;
            sh.kind      = (minor / major >= 0.92f) ? detected_shape::kind_t::circle
                                                    : detected_shape::kind_t::ellipse;
            sh.cx        = ell.center.x / scale;   // 元スケールに戻す
            sh.cy        = ell.center.y / scale;
            sh.rx        = rx / scale;
            sh.ry        = ry / scale;
            sh.angle_deg = ell.angle;
            shapes.push_back(sh);
        }
    }
    set_progress(1.0f);
    return shapes;
}

// ---------------------------------------------------------------------------
// Public analyze API
// ---------------------------------------------------------------------------

void circle_ellipse_tool::analyze(const image_data& img) {
    cancel_requested_.store(false);
    shapes_ = run_detection(img,
        canny_t1, canny_t2, min_radius, max_radius,
        hough_dp, hough_min_dist, hough_param2,
        min_axis_ratio, min_contour_px,
        detect_circles, detect_ellipses,
        max_detect_size, max_fit_error, nullptr, nullptr);
}

void circle_ellipse_tool::start_analyze(const image_data& img) {
    if (is_analyzing()) return;

    cancel_requested_.store(false);
    last_cancelled_ = false;
    analyze_progress_.store(0.0f);
    analyze_future_ = std::async(std::launch::async,
        run_detection,
        img,
        canny_t1, canny_t2, min_radius, max_radius,
        hough_dp, hough_min_dist, hough_param2,
        min_axis_ratio, min_contour_px,
        detect_circles, detect_ellipses,
        max_detect_size, max_fit_error,
        &analyze_progress_, &cancel_requested_);
}

bool circle_ellipse_tool::is_analyzing() const {
    return analyze_future_.valid() &&
           analyze_future_.wait_for(std::chrono::seconds(0))
               != std::future_status::ready;
}

// ---------------------------------------------------------------------------
// Overlay rendering
// ---------------------------------------------------------------------------

void circle_ellipse_tool::draw_rotated_ellipse(ImDrawList* dl, ImVec2 center,
                                                float rx, float ry, float angle_deg,
                                                ImU32 col, float thickness) {
    constexpr int N = 64;
    ImVec2 pts[N];
    const float rad = angle_deg * (static_cast<float>(std::numbers::pi) / 180.0f);
    const float ca  = std::cos(rad);
    const float sa  = std::sin(rad);
    for (int i = 0; i < N; ++i) {
        const float t  = 2.0f * static_cast<float>(std::numbers::pi) * i / N;
        const float lx = rx * std::cos(t);
        const float ly = ry * std::sin(t);
        pts[i] = {center.x + lx * ca - ly * sa,
                  center.y + lx * sa + ly * ca};
    }
    dl->AddPolyline(pts, N, col, ImDrawFlags_Closed, thickness);
}

void circle_ellipse_tool::render_overlay(ImDrawList* dl,
                                          ImVec2 canvas_pos, ImVec2 canvas_size,
                                          float zoom, float pan_x, float pan_y) {
    if (shapes_.empty()) return;

    dl->PushClipRect(canvas_pos,
                     {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y},
                     true);

    for (size_t i = 0; i < shapes_.size(); ++i) {
        const auto& s = shapes_[i];

        const float sx = canvas_pos.x + s.cx * zoom + pan_x;
        const float sy = canvas_pos.y + s.cy * zoom + pan_y;
        const float rx = s.rx * zoom;
        const float ry = s.ry * zoom;

        const ImU32 col    = (s.kind == detected_shape::kind_t::circle)
                             ? IM_COL32(80, 220, 80, 220)
                             : IM_COL32(80, 180, 255, 220);
        const ImU32 lbl_col = IM_COL32(255, 255, 100, 255);

        draw_rotated_ellipse(dl, {sx, sy}, rx, ry, s.angle_deg, col, 1.5f);

        const float cs = std::max(4.0f, 8.0f * zoom);
        dl->AddLine({sx - cs, sy}, {sx + cs, sy}, col, 1.0f);
        dl->AddLine({sx, sy - cs}, {sx, sy + cs}, col, 1.0f);

        {
            const float mrad = s.angle_deg * (static_cast<float>(std::numbers::pi) / 180.0f);
            const float ma   = s.major_semi() * zoom;
            const float cdx  = ma * std::cos(mrad);
            const float cdy  = ma * std::sin(mrad);
            dl->AddLine({sx - cdx, sy - cdy}, {sx + cdx, sy + cdy},
                        IM_COL32(255, 220, 60, 180), 1.0f);
        }

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%zu", i + 1);
        dl->AddText({sx + rx + 4.0f, sy - 8.0f}, lbl_col, buf);
    }

    dl->PopClipRect();
}

// ---------------------------------------------------------------------------
// Panel rendering
// ---------------------------------------------------------------------------

void circle_ellipse_tool::render_panel() {
    // ----- Poll background task -----
    if (analyze_future_.valid() &&
        analyze_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result    = analyze_future_.get();
        last_cancelled_ = cancel_requested_.load();
        if (!last_cancelled_)
            shapes_ = std::move(result);
    }

    // ----- Progress bar + Cancel button -----
    if (is_analyzing()) {
        const float p = analyze_progress_.load();
        char label[32];
        std::snprintf(label, sizeof(label), "Detecting... %d%%",
                      static_cast<int>(p * 100.0f));
        const float btn_w = 70.0f;
        ImGui::ProgressBar(p,
            {ImGui::GetContentRegionAvail().x - btn_w - ImGui::GetStyle().ItemSpacing.x, 0.0f},
            label);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.7f, 0.2f, 0.2f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.9f, 0.3f, 0.3f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.5f, 0.1f, 0.1f, 1.0f});
        if (ImGui::Button("Cancel", {btn_w, 0.0f}))
            cancel_requested_.store(true);
        ImGui::PopStyleColor(3);
        return;
    }

    if (last_cancelled_) {
        ImGui::TextColored({1.0f, 0.5f, 0.3f, 1.0f}, "Cancelled.");
        ImGui::SameLine();
    }

    // ----- Parameters -----
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
    const bool params_open = ImGui::CollapsingHeader("Detection Parameters");
    ImGui::PopStyleColor(3);

    if (params_open) {
        ImGui::Checkbox("Circles (Hough)",      &detect_circles);
        ImGui::SameLine();
        ImGui::Checkbox("Ellipses (Contours)",  &detect_ellipses);

        constexpr ImGuiTableFlags kTf = ImGuiTableFlags_SizingStretchSame
                                      | ImGuiTableFlags_NoPadOuterX;
        if (ImGui::BeginTable("##params", 4, kTf, {ImGui::GetContentRegionAvail().x, 0.0f})) {
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##c1",  &canny_t1,        1.0f,  300.0f, "Canny lo: %.0f");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##c2",  &canny_t2,        1.0f,  500.0f, "Canny hi: %.0f");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##rmin",&min_radius,      1.0f, 1000.0f, "R min: %.0f");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##rmax",&max_radius,      1.0f, 5000.0f, "R max: %.0f");

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##dp",  &hough_dp,        0.5f,    4.0f, "Hough dp: %.2f");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##md",  &hough_min_dist,  1.0f, 1000.0f, "minDist: %.0f");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##p2",  &hough_param2,    5.0f,  100.0f, "Hough acc: %.0f");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##rat", &min_axis_ratio,  0.1f,    1.0f, "Min b/a: %.2f");

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##fe",  &max_fit_error,  0.01f,    1.0f, "Max fit err: %.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Lower = stricter ellipse fit.\n"
                                  "0.05: very strict  0.2: default  0.5: loose");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderInt("##mca",   &min_contour_px,  10, 10000, "Min area: %d px");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1); ImGui::SliderInt("##ds",    &max_detect_size, 128,  4096, "Detect size: %d px");
            ImGui::TableNextColumn();

            ImGui::EndTable();
        }
    }

    ImGui::Separator();

    // ----- Results -----
    ImGui::Text("Detected: %zu shape(s)", shapes_.size());

    if (!shapes_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Show Graph"))
            show_graph_ = !show_graph_;
        ImGui::SameLine();
        if (ImGui::SmallButton("Export CSV")) {
            constexpr nfdfilteritem_t kFilter[] = {{"CSV", "csv"}};
            nfdchar_t* out_path = nullptr;
            if (NFD::SaveDialog(out_path, kFilter, 1, nullptr, "detections.csv") == NFD_OKAY) {
                std::ofstream f(out_path);
                NFD::FreePath(out_path);
                if (f.is_open()) {
                    f << "#,type,cx,cy,major_diam,minor_diam,angle_deg\n";
                    for (size_t i = 0; i < shapes_.size(); ++i) {
                        const auto& s = shapes_[i];
                        f << (i + 1) << ','
                          << (s.kind == detected_shape::kind_t::circle ? "circle" : "ellipse") << ','
                          << s.cx << ',' << s.cy << ','
                          << s.major_diameter() << ',' << s.minor_diameter() << ','
                          << s.angle_deg << '\n';
                    }
                }
            }
        }

        constexpr ImGuiTableFlags kTf =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        const float table_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginTable("##det_tbl", 7, kTf, {0.0f, table_h})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#",           ImGuiTableColumnFlags_WidthFixed,  28.0f);
            ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed,  60.0f);
            ImGui::TableSetupColumn("cx",          ImGuiTableColumnFlags_WidthFixed,  60.0f);
            ImGui::TableSetupColumn("cy",          ImGuiTableColumnFlags_WidthFixed,  60.0f);
            ImGui::TableSetupColumn("Major diam",  ImGuiTableColumnFlags_WidthFixed,  90.0f);
            ImGui::TableSetupColumn("Minor diam",  ImGuiTableColumnFlags_WidthFixed,  90.0f);
            ImGui::TableSetupColumn("Angle (deg)", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < shapes_.size(); ++i) {
                const auto& s = shapes_[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%zu", i + 1);
                ImGui::TableSetColumnIndex(1);
                if (s.kind == detected_shape::kind_t::circle)
                    ImGui::TextColored({0.3f, 0.9f, 0.3f, 1.0f}, "circle");
                else
                    ImGui::TextColored({0.3f, 0.7f, 1.0f, 1.0f}, "ellipse");
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f", static_cast<double>(s.cx));
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f", static_cast<double>(s.cy));
                ImGui::TableSetColumnIndex(4); ImGui::Text("%.2f", static_cast<double>(s.major_diameter()));
                ImGui::TableSetColumnIndex(5); ImGui::Text("%.2f", static_cast<double>(s.minor_diameter()));
                ImGui::TableSetColumnIndex(6); ImGui::Text("%.1f", static_cast<double>(s.angle_deg));
            }
            ImGui::EndTable();
        }
    }

    // ----- Graph window -----
    if (!show_graph_ || shapes_.empty()) return;

    ImGui::SetNextWindowSize({520.0f, 380.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Circle/Ellipse – Major Diameter vs Y", &show_graph_)) {
        ImGui::End();
        return;
    }

    // Split shapes into two series for different colors
    std::vector<double> cx_cir, cy_cir, cx_ell, cy_ell;
    for (const auto& s : shapes_) {
        if (s.kind == detected_shape::kind_t::circle) {
            cx_cir.push_back(static_cast<double>(s.cy));
            cy_cir.push_back(static_cast<double>(s.major_diameter()));
        } else {
            cx_ell.push_back(static_cast<double>(s.cy));
            cy_ell.push_back(static_cast<double>(s.major_diameter()));
        }
    }

    if (ImPlot::BeginPlot("##diam_plot", {-1.0f, -1.0f})) {
        ImPlot::SetupAxes("Image Y (px)", "Major diameter (px)");
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);

        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 5.0f,
                                   ImVec4{0.3f, 0.9f, 0.3f, 0.9f}, 1.0f,
                                   ImVec4{0.3f, 0.9f, 0.3f, 0.9f});
        if (!cx_cir.empty())
            ImPlot::PlotScatter("Circle", cx_cir.data(), cy_cir.data(),
                                static_cast<int>(cx_cir.size()));

        ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond, 5.0f,
                                   ImVec4{0.3f, 0.7f, 1.0f, 0.9f}, 1.0f,
                                   ImVec4{0.3f, 0.7f, 1.0f, 0.9f});
        if (!cx_ell.empty())
            ImPlot::PlotScatter("Ellipse", cx_ell.data(), cy_ell.data(),
                                static_cast<int>(cx_ell.size()));

        ImPlot::EndPlot();
    }

    ImGui::End();
}
