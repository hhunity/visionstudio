#include "gui/remote_overlay_tool.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <nfd.hpp>
#include <imgui.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// State management
// ---------------------------------------------------------------------------

remote_overlay_tool::~remote_overlay_tool() {
    if (worker_.joinable())
        worker_.join();
}

void remote_overlay_tool::set_image_path(const std::string& path) {
    image_path_ = path;
}

bool remote_overlay_tool::is_running() const {
    return state_.load() == remote_tool_state::analyzing;
}

void remote_overlay_tool::clear() {
    if (is_running()) return;
    std::lock_guard lock(result_mtx_);
    entries_.clear();
    error_msg_.clear();
    state_ = remote_tool_state::idle;
}

void remote_overlay_tool::run() {
    if (is_running() || image_path_.empty()) return;
    if (worker_.joinable()) worker_.join();
    state_ = remote_tool_state::analyzing;
    worker_ = std::thread(&remote_overlay_tool::worker_func, this,
                          std::string(host), port, std::string(endpoint),
                          image_path_, std::string(param_path));
}

// ---------------------------------------------------------------------------
// Background worker: POST → read result jsonl
// ---------------------------------------------------------------------------

void remote_overlay_tool::worker_func(std::string host_str, int port_val,
                                       std::string endpoint_str,
                                       std::string image_path,
                                       std::string param_path_str) {
    auto fail = [&](const std::string& msg) {
        std::lock_guard lock(result_mtx_);
        error_msg_ = msg;
        state_ = remote_tool_state::error;
    };

    httplib::Client cli(host_str, port_val);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(60);

    nlohmann::json req;
    req["image_path"] = image_path;
    req["param_path"] = param_path_str;

    auto res = cli.Post(endpoint_str, req.dump(), "application/json");
    if (!res) { fail("Connection failed"); return; }
    if (res->status != 200) { fail("HTTP " + std::to_string(res->status)); return; }

    std::string result_path;
    try {
        auto j    = nlohmann::json::parse(res->body);
        result_path = j.at("result_path").get<std::string>();
    } catch (...) { fail("Invalid response JSON"); return; }

    std::ifstream f(result_path);
    if (!f) { fail("Cannot open: " + result_path); return; }

    std::vector<roi_entry> loaded;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        try {
            auto j  = nlohmann::json::parse(line);
            roi_entry e;
            e.x     = j.at("roi").at("x").get<int>();
            e.y     = j.at("roi").at("y").get<int>();
            e.w     = j.at("roi").at("w").get<int>();
            e.h     = j.at("roi").at("h").get<int>();
            e.dx    = j.at("info").value("dx",    0.0f);
            e.dy    = j.at("info").value("dy",    0.0f);
            e.angle = j.at("info").value("angle", 0.0f);
            e.label = j.value("label", "");
            loaded.push_back(e);
        } catch (...) { /* skip malformed lines */ }
    }

    {
        std::lock_guard lock(result_mtx_);
        entries_ = std::move(loaded);
    }
    state_        = remote_tool_state::done;
    result_ready_ = true;
}

// ---------------------------------------------------------------------------
// Result polling
// ---------------------------------------------------------------------------

bool remote_overlay_tool::poll_result(std::vector<roi_group>& out) {
    bool expected = true;
    if (!result_ready_.compare_exchange_strong(expected, false))
        return false;

    roi_group g;
    g.label = "remote";
    {
        std::lock_guard lock(result_mtx_);
        g.entries = entries_;
    }
    out = {std::move(g)};
    return true;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void remote_overlay_tool::render_panel() {
    // Poll worker state (atomic — safe without lock)
    const remote_tool_state cur = state_.load();

    // Connection settings
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f,0.35f,0.35f,1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f,0.45f,0.45f,1.0f});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f,0.28f,0.28f,1.0f});
    const bool conn_open = ImGui::CollapsingHeader("Connection");
    ImGui::PopStyleColor(3);
    if (conn_open) {
        ImGui::SetNextItemWidth(160); ImGui::InputText("Host",     host,     sizeof(host));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);  ImGui::InputInt ("Port",     &port,    0);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputText("Endpoint", endpoint, sizeof(endpoint));
    }

    ImGui::Separator();

    // Image path (read-only display)
    ImGui::TextDisabled("Image: %s", image_path_.empty() ? "(no image loaded)" : image_path_.c_str());

    // Param JSON path
    const float btn_w   = 80.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btn_w - spacing);
    ImGui::InputText("##param_path", param_path, sizeof(param_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##param", {btn_w, 0})) {
        constexpr nfdfilteritem_t kFilter[] = {{"JSON / JSONL", "json,jsonl"}};
        nfdchar_t* p = nullptr;
        if (NFD::OpenDialog(p, kFilter, 1) == NFD_OKAY) {
            std::strncpy(param_path, p, sizeof(param_path) - 1);
            param_path[sizeof(param_path) - 1] = '\0';
            NFD::FreePath(p);
        }
    }
    ImGui::SameLine(); ImGui::TextDisabled("Params");

    ImGui::Separator();

    // Run / Clear
    const bool can_run = !image_path_.empty() && cur != remote_tool_state::analyzing;
    ImGui::BeginDisabled(!can_run);
    if (ImGui::Button("Run", {btn_w, 0})) run();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(cur == remote_tool_state::analyzing);
    if (ImGui::Button("Clear", {btn_w, 0})) clear();
    ImGui::EndDisabled();

    ImGui::Separator();

    // Status
    switch (cur) {
        case remote_tool_state::idle:
            ImGui::TextDisabled("Idle");
            break;
        case remote_tool_state::analyzing:
            ImGui::TextUnformatted("Analyzing...");
            break;
        case remote_tool_state::done: {
            std::lock_guard lock(result_mtx_);
            ImGui::TextColored({0.4f,1.0f,0.4f,1.0f},
                               "Done — %zu entries", entries_.size());
            break;
        }
        case remote_tool_state::error:
            ImGui::TextColored({1.0f,0.4f,0.4f,1.0f},
                               "Error: %s", error_msg_.c_str());
            break;
    }
}

