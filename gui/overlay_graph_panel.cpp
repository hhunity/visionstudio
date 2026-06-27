#include "gui/overlay_graph_panel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

void overlay_graph_panel::render(const viewer_context& ctx,
                                  image_viewer& single_viewer,
                                  compare_viewer& compare) {
    if (!visible) return;

    ImGui::SetNextWindowSize({800.0f, 400.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Overlay Graph##ovg_panel", &visible)) { ImGui::End(); return; }

    const float avail_w = ImGui::GetContentRegionAvail().x;

    const std::vector<roi_group>* src =
        (ctx.use_single && ctx.overlays) ? ctx.overlays
        : (!ctx.use_single && ctx.left_overlays) ? ctx.left_overlays
        : nullptr;
    const std::vector<uint8_t>*   gvis = nullptr;
    {
        image_viewer& ref = ctx.use_single ? single_viewer : compare.left_viewer_ref();
        if (src && ref.overlay_group_count() == src->size())
            gvis = &ref.overlay_group_visibility;
    }

    if (!src || src->empty()) {
        ImGui::TextDisabled("No overlay loaded");
        ImGui::End();
        return;
    }

    if (!ImGui::BeginTabBar("##ovgtabs")) { ImGui::End(); return; }

    for (size_t gi = 0; gi < src->size(); ++gi) {
        if (gvis && (*gvis)[gi] == 0) continue;
        const auto& g = (*src)[gi];
        if (g.entries.empty()) continue;

        const int n = static_cast<int>(g.entries.size());
        std::vector<double> xs_col(n), xs_row(n), dxs(n), dys(n), angles(n);
        for (int i = 0; i < n; ++i) {
            const auto& e = g.entries[i];
            xs_col[i] = static_cast<double>(e.x);
            xs_row[i] = static_cast<double>(e.y);
            dxs[i]    = static_cast<double>(e.dx);
            dys[i]    = static_cast<double>(e.dy);
            angles[i] = static_cast<double>(e.angle);
        }

        char tab_id[128];
        std::snprintf(tab_id, sizeof(tab_id), "%s##ovgtab%zu", g.label.c_str(), gi);
        if (!ImGui::BeginTabItem(tab_id)) continue;

        bool   y1_force = false, y2_force = false;
        double y1_lo = 0.0, y1_hi = 0.0, y2_lo = 0.0, y2_hi = 0.0;

        if (ImGui::CollapsingHeader("Settings##ovgs")) {
            ImGui::TextUnformatted("Series:");
            ImGui::SameLine(); ImGui::Checkbox("dx##ovs",         &show_dx);
            ImGui::SameLine(); ImGui::Checkbox("dy##ovs",         &show_dy);
            ImGui::SameLine(); ImGui::Checkbox("angle##ovs",      &show_angle);
            ImGui::SameLine(); ImGui::Checkbox("Regression##ovs", &show_fit);

            ImGui::Checkbox("Reference line##ovs", &show_ref);
            if (show_ref) {
                ImGui::SameLine(); ImGui::TextUnformatted("  y =");
                ImGui::SameLine(); ImGui::SetNextItemWidth(80);
                ImGui::InputDouble("##ovg_ra", &ref_a, 0.0, 0.0, "%.4f");
                ImGui::SameLine(); ImGui::TextUnformatted("x +");
                ImGui::SameLine(); ImGui::SetNextItemWidth(80);
                ImGui::InputDouble("##ovg_rb", &ref_b, 0.0, 0.0, "%.4f");
            }

            auto fit_range = [&](const std::vector<double>& a,
                                 const std::vector<double>& b,
                                 bool use_a, bool use_b,
                                 double& lo, double& hi) -> bool {
                lo =  std::numeric_limits<double>::max();
                hi = -std::numeric_limits<double>::max();
                if (use_a && !a.empty()) {
                    lo = std::min(lo, *std::min_element(a.begin(), a.end()));
                    hi = std::max(hi, *std::max_element(a.begin(), a.end()));
                }
                if (use_b && !b.empty()) {
                    lo = std::min(lo, *std::min_element(b.begin(), b.end()));
                    hi = std::max(hi, *std::max_element(b.begin(), b.end()));
                }
                if (lo > hi) return false;
                const double pad = std::max((hi - lo) * 0.05, 1e-9);
                lo -= pad; hi += pad;
                return true;
            };

            ImGui::TextUnformatted("Y1 (dx/dy):");
            ImGui::SameLine();
            if (ImGui::Button("Auto Scale##y1"))
                y1_force = fit_range(dxs, dys, show_dx, show_dy, y1_lo, y1_hi);

            ImGui::TextUnformatted("Y2 (angle):");
            ImGui::SameLine();
            if (ImGui::Button("Auto Scale##y2"))
                y2_force = fit_range(angles, {}, show_angle, false, y2_lo, y2_hi);
        }

        const int stat_lines = (show_fit ? 2 : 0) + (show_ref ? 2 : 0);
        const float text_h = stat_lines > 0
            ? ImGui::GetTextLineHeightWithSpacing() * stat_lines + ImGui::GetStyle().ItemSpacing.y
            : 0.0f;
        const float plot_h = ImGui::GetContentRegionAvail().y - text_h;
        const float plot_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        constexpr ImPlotFlags pf = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;

        auto linreg = [&](const std::vector<double>& xs,
                          const std::vector<double>& ys) -> std::pair<double,double> {
            const int m = static_cast<int>(xs.size());
            if (m < 2) return {0.0, ys.empty() ? 0.0 : ys[0]};
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int i = 0; i < m; ++i) {
                sx += xs[i]; sy += ys[i]; sxx += xs[i]*xs[i]; sxy += xs[i]*ys[i];
            }
            const double denom = m * sxx - sx * sx;
            if (std::abs(denom) < 1e-12) return {0.0, sy / m};
            const double a = (m * sxy - sx * sy) / denom;
            const double b = (sy - a * sx) / m;
            return {a, b};
        };

        struct RefStat { double max_abs, mean, stddev; bool valid; };
        auto compute_ref_stat = [&](const std::vector<double>& xs,
                                    const std::vector<double>& ys) -> RefStat {
            if (!show_ref || n < 1) return {0, 0, 0, false};
            double sum = 0, sum_sq = 0, max_abs = 0;
            for (int i = 0; i < n; ++i) {
                const double e = ys[i] - (ref_a * xs[i] + ref_b);
                sum    += e;
                sum_sq += e * e;
                max_abs = std::max(max_abs, std::abs(e));
            }
            const double mean   = sum / n;
            const double stddev = std::sqrt(std::max(0.0, sum_sq / n - mean * mean));
            return {max_abs, mean, stddev, true};
        };

        const RefStat rs_col_dx = compute_ref_stat(xs_col, dxs);
        const RefStat rs_col_dy = compute_ref_stat(xs_col, dys);
        const RefStat rs_row_dx = compute_ref_stat(xs_row, dxs);
        const RefStat rs_row_dy = compute_ref_stat(xs_row, dys);

        const auto [a_dx_col,  b_dx_col]  = linreg(xs_col, dxs);
        const auto [a_dy_col,  b_dy_col]  = linreg(xs_col, dys);
        const auto [a_ang_col, b_ang_col] = linreg(xs_col, angles);
        const auto [a_dx_row,  b_dx_row]  = linreg(xs_row, dxs);
        const auto [a_dy_row,  b_dy_row]  = linreg(xs_row, dys);
        const auto [a_ang_row, b_ang_row] = linreg(xs_row, angles);

        auto dual_scatter = [&](const char* title,
                                const std::vector<double>& xs,
                                const char* xlabel,
                                double a_dx, double b_dx,
                                double a_dy, double b_dy,
                                double a_ang, double b_ang) {
            char pid[128];
            std::snprintf(pid, sizeof(pid), "%s##%zu", title, gi);

            {
                const ImVec4 wbg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                ImPlot::PushStyleColor(ImPlotCol_PlotBg,
                                       ImVec4(wbg.x, wbg.y, wbg.z, 1.0f));
            }

            if (!ImPlot::BeginPlot(pid, {plot_w, plot_h}, pf)) {
                ImPlot::PopStyleColor();
                return;
            }

            ImPlot::SetupAxes(xlabel, "dx / dy",
                              ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
            ImPlot::SetupAxis(ImAxis_Y2, "angle", ImPlotAxisFlags_None);
            if (y1_force)
                ImPlot::SetupAxisLimits(ImAxis_Y1, y1_lo, y1_hi, ImGuiCond_Always);
            if (y2_force)
                ImPlot::SetupAxisLimits(ImAxis_Y2, y2_lo, y2_hi, ImGuiCond_Always);

            if (show_dx)    ImPlot::PlotScatter("dx",    xs.data(), dxs.data(),    n);
            if (show_dy)    ImPlot::PlotScatter("dy",    xs.data(), dys.data(),    n);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            if (show_angle) ImPlot::PlotScatter("angle", xs.data(), angles.data(), n);

            double xmin = 0.0, xmax = 0.0;
            if ((show_fit || show_ref) && n >= 2) {
                xmin = *std::min_element(xs.begin(), xs.end());
                xmax = *std::max_element(xs.begin(), xs.end());
            }

            if (show_fit && n >= 2) {
                const double fit_xs[2] = {xmin, xmax};
                auto plot_fit = [&](double a, double b, ImAxis yax,
                                    ImVec4 col, const char* lbl) {
                    const double fit_ys[2] = {a*xmin+b, a*xmax+b};
                    ImPlot::SetAxes(ImAxis_X1, yax);
                    ImPlot::SetNextLineStyle(col, 1.5f);
                    char lid[64];
                    std::snprintf(lid, sizeof(lid), "%s##fit_%s_%zu", lbl, lbl, gi);
                    ImPlot::PlotLine(lid, fit_xs, fit_ys, 2);
                };
                if (show_dx)    plot_fit(a_dx,  b_dx,  ImAxis_Y1, {0.4f,0.8f,1.0f,0.8f}, "dx fit");
                if (show_dy)    plot_fit(a_dy,  b_dy,  ImAxis_Y1, {0.4f,1.0f,0.5f,0.8f}, "dy fit");
                if (show_angle) plot_fit(a_ang, b_ang, ImAxis_Y2, {1.0f,0.7f,0.3f,0.8f}, "angle fit");
            }

            if (show_ref && n >= 2) {
                const double ref_xs[2] = {xmin, xmax};
                const double ref_ys[2] = {ref_a*xmin + ref_b, ref_a*xmax + ref_b};
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::SetNextLineStyle({1.0f, 0.9f, 0.0f, 1.0f}, 2.0f);
                char rid[64];
                std::snprintf(rid, sizeof(rid), "y=%.3fx%+.3f##ref_%zu", ref_a, ref_b, gi);
                ImPlot::PlotLine(rid, ref_xs, ref_ys, 2, ImPlotItemFlags_NoFit);
            }

            if (ImPlot::IsPlotHovered() && n > 0) {
                const ImVec2 mp = ImGui::GetMousePos();
                int nearest = -1, nearest_series = -1;
                float best = 15.0f;
                for (int i = 0; i < n; ++i) {
                    auto check = [&](double y, ImAxis ya, int sid) {
                        const ImVec2 pt = ImPlot::PlotToPixels(xs[i], y, ImAxis_X1, ya);
                        const float d = std::sqrt((pt.x-mp.x)*(pt.x-mp.x) +
                                                   (pt.y-mp.y)*(pt.y-mp.y));
                        if (d < best) { best = d; nearest = i; nearest_series = sid; }
                    };
                    if (show_dx)    check(dxs[i],    ImAxis_Y1, 0);
                    if (show_dy)    check(dys[i],    ImAxis_Y1, 1);
                    if (show_angle) check(angles[i], ImAxis_Y2, 2);
                }
                if (nearest >= 0) {
                    const auto& e = g.entries[nearest];
                    ImGui::BeginTooltip();
                    ImGui::Text("x: %d  y: %d", e.x, e.y);
                    ImGui::Separator();
                    auto row_text = [&](const char* label, double val, bool highlight) {
                        if (highlight)
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 60, 255));
                        ImGui::Text("%-6s %.6f", label, val);
                        if (highlight) ImGui::PopStyleColor();
                    };
                    if (show_dx)    row_text("dx:",    e.dx,    nearest_series == 0);
                    if (show_dy)    row_text("dy:",    e.dy,    nearest_series == 1);
                    if (show_angle) row_text("angle:", e.angle, nearest_series == 2);
                    ImGui::EndTooltip();
                }
            }

            ImPlot::EndPlot();
            ImPlot::PopStyleColor();
        };

        if (plot_h > 1.0f && plot_w > 1.0f) {
            dual_scatter("Column", xs_col, "col",
                         a_dx_col, b_dx_col, a_dy_col, b_dy_col, a_ang_col, b_ang_col);
            ImGui::SameLine();
            dual_scatter("Row", xs_row, "row",
                         a_dx_row, b_dx_row, a_dy_row, b_dy_row, a_ang_row, b_ang_row);
        } else {
            ImGui::TextDisabled("(enlarge panel to view plots)");
        }

        if (show_fit) {
            ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Col:");
            ImGui::SameLine(); ImGui::Text("dx=%.4fx%+.4f", a_dx_col, b_dx_col);
            ImGui::SameLine(); ImGui::Text("  dy=%.4fx%+.4f", a_dy_col, b_dy_col);
            ImGui::SameLine();
            ImGui::TextColored({1.0f,0.7f,0.3f,1.0f},
                               "  angle=%.4fx%+.4f", a_ang_col, b_ang_col);

            ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Row:");
            ImGui::SameLine(); ImGui::Text("dx=%.4fx%+.4f", a_dx_row, b_dx_row);
            ImGui::SameLine(); ImGui::Text("  dy=%.4fx%+.4f", a_dy_row, b_dy_row);
            ImGui::SameLine();
            ImGui::TextColored({1.0f,0.7f,0.3f,1.0f},
                               "  angle=%.4fx%+.4f", a_ang_row, b_ang_row);
        }

        if (show_ref) {
            constexpr ImVec4 kRefCol = {1.0f, 0.9f, 0.0f, 1.0f};
            auto show_stat = [](const char* label, const RefStat& s) {
                ImGui::SameLine();
                ImGui::Text("%s max=%.4f  mean=%+.4f  σ=%.4f",
                            label, s.max_abs, s.mean, s.stddev);
            };
            ImGui::TextColored(kRefCol, "Ref Col:");
            if (show_dx) show_stat("dx:", rs_col_dx);
            if (show_dy) show_stat("dy:", rs_col_dy);

            ImGui::TextColored(kRefCol, "Ref Row:");
            if (show_dx) show_stat("dx:", rs_row_dx);
            if (show_dy) show_stat("dy:", rs_row_dy);
        }

        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

void overlay_graph_panel::load_settings(const nlohmann::json& j) {
    if (!j.contains("overlay_graph") || !j["overlay_graph"].is_object()) return;
    const auto& og = j["overlay_graph"];
    auto gb = [&](const char* k, bool&   v){ if (og.contains(k) && og[k].is_boolean()) v = og[k]; };
    auto gd = [&](const char* k, double& v){ if (og.contains(k) && og[k].is_number())  v = og[k]; };
    gb("show_dx",    show_dx);
    gb("show_dy",    show_dy);
    gb("show_angle", show_angle);
    gb("show_fit",   show_fit);
    gb("show_ref",   show_ref);
    gd("ref_a",      ref_a);
    gd("ref_b",      ref_b);
}

nlohmann::json overlay_graph_panel::save_settings() const {
    return {
        {"show_dx",    show_dx},
        {"show_dy",    show_dy},
        {"show_angle", show_angle},
        {"show_fit",   show_fit},
        {"show_ref",   show_ref},
        {"ref_a",      ref_a},
        {"ref_b",      ref_b},
    };
}
