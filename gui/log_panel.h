#pragma once
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <imgui.h>
#include "gui/panel_base.h"

struct log_panel : public panel_base {
    struct entry { std::string text; bool is_error; };
    std::vector<entry> entries;
    bool               scroll_to_bottom = true;
    // visible is inherited from panel_base

    void add(const char* level, const char* msg) {
        const auto now = std::chrono::system_clock::now();
        const auto t   = std::chrono::system_clock::to_time_t(now);
        const int  ms  = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count() % 1000);
        struct tm tm_info{};
#ifdef _WIN32
        localtime_s(&tm_info, &t);
#else
        localtime_r(&t, &tm_info);
#endif
        char tbuf[16];
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
        char line[512];
        std::snprintf(line, sizeof(line), "[%s.%03d] %-5s %s", tbuf, ms, level, msg);
        entries.push_back({line, std::strcmp(level, "ERROR") == 0});
        scroll_to_bottom = true;
    }

    void render() override {
        if (!visible) return;
        if (!ImGui::Begin("Log##log_win", &visible)) { ImGui::End(); return; }
        if (ImGui::Button("Clear")) entries.clear();
        ImGui::Separator();
        ImGui::BeginChild("scrolling", {0, 0}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& e : entries) {
            if (e.is_error)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextUnformatted(e.text.c_str());
            if (e.is_error) ImGui::PopStyleColor();
        }
        if (scroll_to_bottom) { ImGui::SetScrollHereY(1.0f); scroll_to_bottom = false; }
        ImGui::EndChild();
        ImGui::End();
    }
};
