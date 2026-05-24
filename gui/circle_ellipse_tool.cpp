#include "gui/circle_ellipse_tool.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <imgui.h>
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

std::vector<detected_shape> circle_ellipse_tool::run_detection(
    const image_data& img,
    float canny_t1, float canny_t2,
    float min_radius, float max_radius,
    float hough_dp, float hough_min_dist, float hough_param2,
    float min_axis_ratio, int min_contour_px,
    bool detect_circles, bool detect_ellipses,
    std::atomic<float>* progress)
{
    auto set_progress = [&](float v) { if (progress) progress->store(v); };

    std::vector<detected_shape> shapes;
    if (img.empty()) return shapes;

    set_progress(0.0f);
    cv::Mat gray = to_gray_mat(img);
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, {5, 5}, 1.5);
    set_progress(0.1f);

    // ----- HoughCircles -----
    if (detect_circles) {
        std::vector<cv::Vec3f> circles;
        cv::HoughCircles(blurred, circles, cv::HOUGH_GRADIENT,
                         static_cast<double>(hough_dp),
                         static_cast<double>(hough_min_dist),
                         static_cast<double>(canny_t2),
                         static_cast<double>(hough_param2),
                         static_cast<int>(min_radius),
                         static_cast<int>(max_radius));
        for (const auto& c : circles) {
            detected_shape s;
            s.kind      = detected_shape::kind_t::circle;
            s.cx        = c[0];
            s.cy        = c[1];
            s.rx        = c[2];
            s.ry        = c[2];
            s.angle_deg = 0.0f;
            shapes.push_back(s);
        }
    }
    set_progress(0.4f);

    // ----- Contour-based ellipse fitting -----
    if (detect_ellipses) {
        cv::Mat edges;
        cv::Canny(blurred, edges, canny_t1, canny_t2);
        set_progress(0.55f);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_NONE);

        const float n = static_cast<float>(contours.size());
        for (size_t ci = 0; ci < contours.size(); ++ci) {
            // 輪郭ループは本物の進捗（55%→100%）
            set_progress(0.55f + 0.45f * (static_cast<float>(ci) / std::max(n, 1.0f)));

            const auto& contour = contours[ci];
            if (contour.size() < 5) continue;
            if (cv::contourArea(contour) < min_contour_px) continue;

            const cv::RotatedRect ell = cv::fitEllipse(contour);
            const float rx = ell.size.width  * 0.5f;
            const float ry = ell.size.height * 0.5f;
            if (rx <= 0.0f || ry <= 0.0f) continue;

            const float major = std::max(rx, ry);
            const float minor = std::min(rx, ry);
            if (major < min_radius || major > max_radius) continue;
            if (minor / major < min_axis_ratio) continue;

            bool dup = false;
            for (const auto& existing : shapes) {
                const float dx = ell.center.x - existing.cx;
                const float dy = ell.center.y - existing.cy;
                if (std::sqrt(dx * dx + dy * dy) < hough_min_dist * 0.5f) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            detected_shape s;
            s.kind      = (minor / major >= 0.92f) ? detected_shape::kind_t::circle
                                                   : detected_shape::kind_t::ellipse;
            s.cx        = ell.center.x;
            s.cy        = ell.center.y;
            s.rx        = rx;
            s.ry        = ry;
            s.angle_deg = ell.angle;
            shapes.push_back(s);
        }
    }
    set_progress(1.0f);
    return shapes;
}

// ---------------------------------------------------------------------------
// Public analyze API
// ---------------------------------------------------------------------------

void circle_ellipse_tool::analyze(const image_data& img) {
    shapes_ = run_detection(img,
        canny_t1, canny_t2, min_radius, max_radius,
        hough_dp, hough_min_dist, hough_param2,
        min_axis_ratio, min_contour_px,
        detect_circles, detect_ellipses,
        nullptr);
}

void circle_ellipse_tool::start_analyze(const image_data& img) {
    if (is_analyzing()) return;  // skip if already running

    analyze_progress_.store(0.0f);
    analyze_future_ = std::async(std::launch::async,
        run_detection,
        img,  // copied into the async task
        canny_t1, canny_t2, min_radius, max_radius,
        hough_dp, hough_min_dist, hough_param2,
        min_axis_ratio, min_contour_px,
        detect_circles, detect_ellipses,
        &analyze_progress_);
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
        shapes_ = analyze_future_.get();
    }

    // ----- Progress bar -----
    if (is_analyzing()) {
        const float p = analyze_progress_.load();
        char label[32];
        std::snprintf(label, sizeof(label), "Detecting... %d%%",
                      static_cast<int>(p * 100.0f));
        ImGui::ProgressBar(p, {-1.0f, 0.0f}, label);
        return;
    }

    // ----- Parameters -----
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
    const bool params_open = ImGui::CollapsingHeader("Detection Parameters");
    ImGui::PopStyleColor(3);

    if (params_open) {
        const float fw = ImGui::GetContentRegionAvail().x;
        ImGui::Checkbox("Circles (Hough)",      &detect_circles);
        ImGui::SameLine();
        ImGui::Checkbox("Ellipses (Contours)",  &detect_ellipses);

        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderFloat("Canny lo##c1",   &canny_t1,        1.0f,  300.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderFloat("Canny hi##c2",   &canny_t2,        1.0f,  500.0f);

        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderFloat("R min##rmin",    &min_radius,      1.0f, 1000.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderFloat("R max##rmax",    &max_radius,      1.0f, 5000.0f);

        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderFloat("Hough dp##dp",   &hough_dp,        0.5f,    4.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderFloat("Hough minDist",  &hough_min_dist,  1.0f, 1000.0f);

        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderFloat("Hough acc##p2",  &hough_param2,    5.0f,  100.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderFloat("Min b/a##rat",   &min_axis_ratio,  0.1f,    1.0f);

        ImGui::SetNextItemWidth(fw * 0.45f);
        ImGui::SliderInt("Min area (px)", &min_contour_px, 10, 10000);
    }

    ImGui::Separator();

    // ----- Results -----
    ImGui::Text("Detected: %zu shape(s)", shapes_.size());

    if (!shapes_.empty()) {
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
}
