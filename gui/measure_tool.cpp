#include "gui/measure_tool.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

const ImU32 measure_tool::kColors[measure_tool::kMaxEntries] = {
    IM_COL32(255, 220,  50, 255),  // yellow
    IM_COL32( 50, 210, 255, 255),  // cyan
    IM_COL32( 80, 255, 120, 255),  // green
    IM_COL32(255, 140,  50, 255),  // orange
    IM_COL32(255, 100, 180, 255),  // pink
};

ImVec2 measure_tool::to_screen(float img_x, float img_y,
                                ImVec2 canvas_pos, float zoom,
                                float pan_x, float pan_y) const {
    return {canvas_pos.x + pan_x + img_x * zoom,
            canvas_pos.y + pan_y + img_y * zoom};
}

void measure_tool::handle_click(int img_x, int img_y) {
    if (!pending_) {
        if (static_cast<int>(entries_.size()) >= kMaxEntries) return;
        pending_ = entry{static_cast<float>(img_x), static_cast<float>(img_y)};
    } else {
        pending_->pt2_x = static_cast<float>(img_x);
        pending_->pt2_y = static_cast<float>(img_y);
        entries_.push_back(*pending_);
        pending_.reset();
    }
}

void measure_tool::reset() {
    entries_.clear();
    pending_.reset();
}

void measure_tool::draw_entry(ImDrawList* dl, const entry& e, ImU32 col,
                               ImVec2 canvas_pos, float zoom,
                               float pan_x, float pan_y) const {
    constexpr ImU32 kColDim    = IM_COL32(160, 210, 255, 210);
    constexpr ImU32 kColTextBg = IM_COL32(0,   0,   0,  160);
    const     ImU32 kColLine   = (col & 0x00FFFFFFu) | 0xC8000000u;

    const ImVec2 s1 = to_screen(e.pt1_x, e.pt1_y, canvas_pos, zoom, pan_x, pan_y);
    const ImVec2 s2 = to_screen(e.pt2_x, e.pt2_y, canvas_pos, zoom, pan_x, pan_y);
    const float  dx = e.pt2_x - e.pt1_x;
    const float  dy = e.pt2_y - e.pt1_y;

    dl->AddLine(s1, s2, kColLine, 1.5f);
    dl->AddCircleFilled(s1, 5.0f, col);
    dl->AddCircle(s1, 5.0f, IM_COL32(0, 0, 0, 180), 0, 1.5f);
    dl->AddCircleFilled(s2, 5.0f, col);
    dl->AddCircle(s2, 5.0f, IM_COL32(0, 0, 0, 180), 0, 1.5f);

    const ImVec2 corner = {s2.x, s1.y};
    dl->AddLine(s1,     corner, kColDim, 1.0f);
    dl->AddLine(corner, s2,     kColDim, 1.0f);

    // dx label (above horizontal helper)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "dx: %.1f px", std::abs(dx));
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 lp = {(s1.x + corner.x) * 0.5f - ts.x * 0.5f,
                            std::min(s1.y, corner.y) - ts.y - 3.0f};
        dl->AddRectFilled({lp.x-2, lp.y-1}, {lp.x+ts.x+2, lp.y+ts.y+1}, kColTextBg);
        dl->AddText(lp, kColDim, buf);
    }

    // dy label (right of vertical helper)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "dy: %.1f px", std::abs(dy));
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 lp = {std::max(corner.x, s2.x) + 5.0f,
                            (corner.y + s2.y) * 0.5f - ts.y * 0.5f};
        dl->AddRectFilled({lp.x-2, lp.y-1}, {lp.x+ts.x+2, lp.y+ts.y+1}, kColTextBg);
        dl->AddText(lp, kColDim, buf);
    }

    // angle (above midpoint) + length (below midpoint)
    {
        const double angle_deg = std::atan2(static_cast<double>(dy),
                                            static_cast<double>(dx))
                                 * (180.0 / std::numbers::pi);
        const float len = std::sqrt(dx * dx + dy * dy);

        char abuf[48], lbuf[48];
        std::snprintf(abuf, sizeof(abuf), "%.2f deg", angle_deg);
        std::snprintf(lbuf, sizeof(lbuf), "%.2f px",  len);

        const ImVec2 ats   = ImGui::CalcTextSize(abuf);
        const ImVec2 lts   = ImGui::CalcTextSize(lbuf);
        const float  mid_x = (s1.x + s2.x) * 0.5f;
        const float  mid_y = (s1.y + s2.y) * 0.5f;

        const ImVec2 alp = {mid_x - ats.x * 0.5f, mid_y - ats.y - 4.0f};
        dl->AddRectFilled({alp.x-2, alp.y-1}, {alp.x+ats.x+2, alp.y+ats.y+1}, kColTextBg);
        dl->AddText(alp, col, abuf);

        const ImVec2 llp = {mid_x - lts.x * 0.5f, mid_y + 4.0f};
        dl->AddRectFilled({llp.x-2, llp.y-1}, {llp.x+lts.x+2, llp.y+lts.y+1}, kColTextBg);
        dl->AddText(llp, kColDim, lbuf);
    }
}

void measure_tool::render_overlay(ImDrawList* dl,
                                   ImVec2 canvas_pos, ImVec2 canvas_size,
                                   float zoom, float pan_x, float pan_y,
                                   int hover_img_x, int hover_img_y) {
    if (entries_.empty() && !pending_) return;

    dl->PushClipRect(canvas_pos,
                     {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y}, true);

    for (int i = 0; i < static_cast<int>(entries_.size()); ++i)
        draw_entry(dl, entries_[i], kColors[i], canvas_pos, zoom, pan_x, pan_y);

    if (pending_) {
        const ImU32  col  = kColors[static_cast<int>(entries_.size())];
        const ImU32  col_line = (col & 0x00FFFFFFu) | 0xC8000000u;
        const ImVec2 s1   = to_screen(pending_->pt1_x, pending_->pt1_y,
                                      canvas_pos, zoom, pan_x, pan_y);
        dl->AddCircleFilled(s1, 5.0f, col);
        dl->AddCircle(s1, 5.0f, IM_COL32(0, 0, 0, 180), 0, 1.5f);

        if (hover_img_x >= 0 && hover_img_y >= 0) {
            const ImVec2 sh = to_screen(static_cast<float>(hover_img_x),
                                        static_cast<float>(hover_img_y),
                                        canvas_pos, zoom, pan_x, pan_y);
            constexpr ImU32 kColDim = IM_COL32(160, 210, 255, 140);
            dl->AddLine(s1, sh, col_line, 1.5f);
            dl->AddLine(s1,           {sh.x, s1.y}, kColDim, 1.0f);
            dl->AddLine({sh.x, s1.y}, sh,            kColDim, 1.0f);
        }
    }

    dl->PopClipRect();
}

void measure_tool::render_panel() {
    const int  n           = static_cast<int>(entries_.size());
    const bool has_pending = pending_.has_value();

    if (n == 0 && !has_pending) {
        ImGui::TextDisabled("Click point 1 on the image");
        return;
    }

    int erase_idx = -1;
    for (int i = 0; i < n; ++i) {
        const auto& e = entries_[i];
        const float dx  = e.pt2_x - e.pt1_x;
        const float dy  = e.pt2_y - e.pt1_y;
        const float len = std::sqrt(dx * dx + dy * dy);
        const double angle_deg = std::atan2(static_cast<double>(dy),
                                            static_cast<double>(dx))
                                 * (180.0 / std::numbers::pi);

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kColors[i]));
        char hdr[24];
        std::snprintf(hdr, sizeof(hdr), "Measure %d", i + 1);
        const bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();

        if (open) {
            ImGui::Indent(8.0f);
            ImGui::Text("pt1: (%.0f, %.0f)", e.pt1_x, e.pt1_y);
            ImGui::Text("pt2: (%.0f, %.0f)", e.pt2_x, e.pt2_y);
            ImGui::Text("|dx|:   %.1f px",   std::abs(dx));
            ImGui::Text("|dy|:   %.1f px",   std::abs(dy));
            ImGui::Text("length: %.2f px",   len);
            ImGui::Text("angle:  %.4f deg",  angle_deg);
            char del_id[32];
            std::snprintf(del_id, sizeof(del_id), "Delete##%d", i);
            if (ImGui::Button(del_id, {-1.0f, 0.0f})) erase_idx = i;
            ImGui::Unindent(8.0f);
        }
    }

    if (erase_idx >= 0)
        entries_.erase(entries_.begin() + erase_idx);

    ImGui::Separator();
    if (has_pending)
        ImGui::TextDisabled("Click point 2 on the image");
    else if (n < kMaxEntries)
        ImGui::TextDisabled("Click pt1 to add  (%d / %d)", n, kMaxEntries);
    else
        ImGui::TextDisabled("Max %d measurements reached", kMaxEntries);

    if (ImGui::Button("Reset All", {-1.0f, 0.0f})) reset();
}
