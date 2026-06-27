#include "gui/profile_panel.h"
#include "gui/app_context.h"
#include "gui/image_viewer.h"
#include "gui/compare_viewer.h"
#include "io/overlay_io.h"
#include <algorithm>
#include <cstdio>
#include <vector>
#include <implot.h>

void profile_panel::render() {
    if (!visible) return;

    ImGui::SetNextWindowSize({640.0f, 200.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Profile##profile_panel", &visible)) { ImGui::End(); return; }

    const float graph_h = ImGui::GetContentRegionAvail().y;
    const float graph_w = (ImGui::GetContentRegionAvail().x
                           - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    if (ctx_->use_single && ctx_->single_viewer) {
        const auto& hi  = ctx_->single_viewer->get_hover_info();
        const auto* img = &ctx_->single_viewer->get_image_data();
        const int cx = hi.valid ? hi.img_x : -1;
        const int cy = hi.valid ? hi.img_y : -1;
        const auto& vs = ctx_->single_viewer->get_view_state();
        const int vis_x0 = static_cast<int>(-vs.pan_x / vs.zoom);
        const int vis_x1 = static_cast<int>((-vs.pan_x + ctx_->viewer_w) / vs.zoom);
        const int vis_y0 = static_cast<int>(-vs.pan_y / vs.zoom);
        const int vis_y1 = static_cast<int>((-vs.pan_y + ctx_->viewer_h) / vs.zoom);
        draw_profile("X Profile##xprof", true,  cy,
                     {{img, IM_COL32(80, 200, 255, 220), cx}},
                     graph_w, graph_h, vis_x0, vis_x1);
        ImGui::SameLine();
        draw_profile("Y Profile##yprof", false, cx,
                     {{img, IM_COL32(80, 200, 255, 220), cy}},
                     graph_w, graph_h, vis_y0, vis_y1);
    } else if (!ctx_->use_single && ctx_->compare) {
        const auto& hi  = ctx_->compare->get_hover_info();
        const auto* li  = &ctx_->compare->get_left_image_data();
        const auto* ri  = &ctx_->compare->get_right_image_data();
        const int cx = hi.valid ? hi.img_x : -1;
        const int cy = hi.valid ? hi.img_y : -1;
        const auto& vs = ctx_->compare->get_view_state();
        const int vis_x0 = static_cast<int>(-vs.pan_x / vs.zoom);
        const int vis_x1 = static_cast<int>((-vs.pan_x + ctx_->viewer_w * 0.5f) / vs.zoom);
        const int vis_y0 = static_cast<int>(-vs.pan_y / vs.zoom);
        const int vis_y1 = static_cast<int>((-vs.pan_y + ctx_->viewer_h) / vs.zoom);
        draw_profile("X Profile##xprof", true, cy,
                     {{li, IM_COL32(80, 200, 255, 220), cx},
                      {ri, IM_COL32(255, 160,  60, 220), cx}},
                     graph_w, graph_h, vis_x0, vis_x1);
        ImGui::SameLine();
        draw_profile("Y Profile##yprof", false, cx,
                     {{li, IM_COL32(80, 200, 255, 220), cy},
                      {ri, IM_COL32(255, 160,  60, 220), cy}},
                     graph_w, graph_h, vis_y0, vis_y1);
    }

    ImGui::End();
}

void profile_panel::draw_profile(const char* plot_id, bool is_x, int fixed,
                                  std::initializer_list<series_entry> series,
                                  float gw, float gh,
                                  int vis_min, int vis_max) {
    int total = 0;
    for (const auto& s : series) {
        if (s.img && !s.img->empty()) {
            total = is_x ? s.img->width : s.img->height;
            break;
        }
    }

    const bool has_vis = vis_min >= 0 && vis_max > vis_min;

    constexpr ImPlotFlags kPlotFlags =
        ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText;
    constexpr ImPlotAxisFlags kAxFlags =
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels;

    if (!ImPlot::BeginPlot(plot_id, {gw, gh}, kPlotFlags)) return;

    ImPlot::SetupAxes(nullptr, nullptr, kAxFlags, kAxFlags);
    ImPlot::SetupAxisLimits(ImAxis_X1, 0, total > 1 ? total - 1 : 1, ImGuiCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 255, ImGuiCond_Always);

    int sidx = 0;
    for (const auto& s : series) {
        if (!s.img || s.img->empty() || fixed < 0 || total <= 0) { ++sidx; continue; }
        const int n = is_x ? s.img->width : s.img->height;
        std::vector<float> ys(n);
        for (int i = 0; i < n; ++i) {
            const auto px = is_x ? s.img->pixel_at(i, fixed) : s.img->pixel_at(fixed, i);
            ys[i] = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
        }

        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "##lum%d", sidx);
        ImPlot::SetNextLineStyle(ImGui::ColorConvertU32ToFloat4(s.color), 1.0f);
        ImPlot::PlotLine(lbl, ys.data(), n);

        if (has_vis) {
            const int v0 = std::max(0, vis_min);
            const int v1 = std::min(n - 1, vis_max);
            const int m  = v1 - v0 + 1;
            if (m > 1) {
                std::vector<float> vis_ys(ys.begin() + v0, ys.begin() + v1 + 1);
                const double xscale = static_cast<double>(total - 1) / (m - 1);
                char vlbl[16];
                std::snprintf(vlbl, sizeof(vlbl), "##vis%d", sidx);
                ImPlot::SetNextLineStyle(
                    ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 220, 60, 200)), 1.5f);
                ImPlot::PlotLine(vlbl, vis_ys.data(), m, xscale, 0.0);
            }
        }

        if (s.cursor >= 0 && s.cursor < n) {
            const double cx[2] = {static_cast<double>(s.cursor),
                                  static_cast<double>(s.cursor)};
            const double cy[2] = {0.0, 255.0};
            char clbl[16];
            std::snprintf(clbl, sizeof(clbl), "##cur%d", sidx);
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.31f, 0.31f, 0.9f), 1.5f);
            ImPlot::PlotLine(clbl, cx, cy, 2);
        }
        ++sidx;
    }

    ImPlot::EndPlot();
}
