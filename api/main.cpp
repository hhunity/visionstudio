#include <glad/glad.h>
#include <nfd.hpp>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#include "capture/capture_client.h"
#include "util/capture_config.h"
#include "gui/circle_ellipse_tool.h"
#include "gui/remote_overlay_tool.h"
#include "gui/compare_viewer.h"
#include "gui/image_viewer.h"
#include <implot.h>
#include "io/overlay_io.h"
#include "external/cpplib/io/tiff_io.h"

#include "generated/third_party_licenses.h"
#include "generated/version.h"
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
static void fatal_error(const char* msg) {
    MessageBoxA(nullptr, msg, "VisionStudio - Fatal Error", MB_OK | MB_ICONERROR);
}
#else
static void fatal_error(const char* msg) { fprintf(stderr, "Fatal: %s\n", msg); }
#endif

static void glfw_error_cb(int error, const char* desc) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------

enum class view_mode  { none, single, compare };
enum class input_mode { read_img, remote_capture };

// ---------------------------------------------------------------------------
// Async loader
// ---------------------------------------------------------------------------

struct async_loader {
    std::future<image_data> future;
    std::atomic<float>      progress{0.0f};
    std::atomic<bool>       cancel{false};
    bool                    active = false;
    std::string             path;

    async_loader()                               = default;
    async_loader(const async_loader&)            = delete;
    async_loader& operator=(const async_loader&) = delete;

    ~async_loader() {
        cancel.store(true);
        if (future.valid()) future.wait();
    }

    void start(std::string p) {
        path = p;
        progress.store(0.0f);
        cancel.store(false);
        active = true;
        auto* prog = &progress;
        future = std::async(std::launch::async, [p = std::move(p), prog]() {
            image_data img;
            const PixelFormat native = tiff_io::detect_format(p);
            const PixelFormat fmt    = (native == PixelFormat::gray) ? PixelFormat::gray
                                                                      : PixelFormat::rgba;
            if (!tiff_io::read(p, img, prog, tiff_io::ReadOptions{.output_format = fmt}))
                img.pixels.clear(); // ensure empty() == true so caller knows load failed
            return img;
        });
    }

    bool poll(image_data& out) {
        if (!active) return false;
        if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        out    = future.get();
        active = false;
        return true;
    }
};

// ---------------------------------------------------------------------------
// App state shared with GLFW drop callback
// ---------------------------------------------------------------------------

struct app_state {
    view_mode*               vmode         = nullptr;
    input_mode*              imode         = nullptr;
    image_viewer*            single_viewer = nullptr;
    compare_viewer*          compare       = nullptr;
    image_data*              left_image    = nullptr;
    image_data*              right_image   = nullptr;
    std::string*             status_msg    = nullptr;
    async_loader*            left_loader   = nullptr;
    async_loader*            right_loader  = nullptr;
    std::vector<roi_group>*  overlays       = nullptr; // single mode
    std::vector<roi_group>*  left_overlays  = nullptr; // compare / split mode (left panel)
    std::vector<roi_group>*  right_overlays = nullptr; // compare mode (right panel)
    std::string*             overlay_file       = nullptr; // single / split
    std::string*             left_overlay_file  = nullptr; // compare left
    std::string*             right_overlay_file = nullptr; // compare right
};

// ---------------------------------------------------------------------------
// Config tab — text file editor state (path, text content, modified flag).
// ---------------------------------------------------------------------------

struct config_tab {
    std::string path;
    std::string text;
    bool        modified = false;

    void load() {
        std::ifstream f(path);
        if (!f.is_open()) return;
        text = std::string(std::istreambuf_iterator<char>(f), {});
        modified = false;
    }
    void save() {
        std::ofstream f(path);
        if (!f.is_open()) return;
        f << text;
        modified = false;
    }
};

// ---------------------------------------------------------------------------
// Log window
// ---------------------------------------------------------------------------

struct AppLog {
    struct Entry { std::string text; bool is_error; };
    std::vector<Entry> entries;
    bool               scroll_to_bottom = true;

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

    void draw(const char* title, bool* p_open) {
        if (!ImGui::Begin(title, p_open)) { ImGui::End(); return; }
        if (ImGui::Button("Clear")) entries.clear();
        ImGui::Separator();
        ImGui::BeginChild("scrolling", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& e : entries) {
            if (e.is_error) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextUnformatted(e.text.c_str());
            if (e.is_error) ImGui::PopStyleColor();
        }
        if (scroll_to_bottom) { ImGui::SetScrollHereY(1.0f); scroll_to_bottom = false; }
        ImGui::EndChild();
        ImGui::End();
    }
};

// ---------------------------------------------------------------------------
// Toggle switch widget (ImGui has no built-in)
// ---------------------------------------------------------------------------

static bool toggle_switch(const char* str_id, bool* v) {
    const float h = ImGui::GetFrameHeight() * 0.85f;
    const float w = h * 1.8f;
    const float r = h * 0.5f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(str_id, {w, h});
    const bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = *v ? 1.0f : 0.0f;
    const ImU32 track = ImGui::IsItemHovered()
        ? (*v ? IM_COL32( 80,190, 80,255) : IM_COL32(190,190,190,255))
        : (*v ? IM_COL32( 66,170, 66,255) : IM_COL32(160,160,160,255));
    dl->AddRectFilled(p, {p.x + w, p.y + h}, track, r);
    dl->AddCircleFilled({p.x + r + t * (w - r * 2.0f), p.y + r}, r - 2.0f, IM_COL32(255,255,255,255));
    return clicked;
}

// ---------------------------------------------------------------------------
// Static inline helpers shared between drop_callback and the main loop.
// ---------------------------------------------------------------------------

// Load an overlay JSON file into `buf`, call `setter(buf)`, and optionally
// update `file_out` and `status_out`. Returns true on success.
template<typename Setter>
static inline bool load_overlay(const std::string& path,
                                 std::vector<roi_group>& buf,
                                 Setter setter,
                                 std::string* file_out    = nullptr,
                                 std::string* status_out  = nullptr,
                                 const char*  status_prefix = "Overlay loaded: ") {
    if (!overlay_io::load(path, buf)) return false;
    setter(buf);
    if (file_out)   *file_out   = path;
    if (status_out) *status_out = std::string(status_prefix) + path;
    return true;
}

// Render ImGui checkboxes for each overlay group in `viewer` (for panels).
// If `sync_viewer` is non-null its visibility is kept in sync (split mode).
static inline void overlay_group_checkboxes(image_viewer& viewer,
                                             const char* id_prefix,
                                             image_viewer* sync_viewer = nullptr) {
    for (size_t gi = 0; gi < viewer.overlay_group_count(); ++gi) {
        char id[128];
        std::snprintf(id, sizeof(id), "%s##%s%zu",
                      viewer.overlay_group_label(gi).c_str(), id_prefix, gi);
        bool vis = viewer.overlay_group_visibility[gi] != 0;
        if (ImGui::Checkbox(id, &vis)) {
            viewer.overlay_group_visibility[gi] = vis ? 1 : 0;
            if (sync_viewer && gi < sync_viewer->overlay_group_visibility.size())
                sync_viewer->overlay_group_visibility[gi] = vis ? 1 : 0;
        }
    }
}

// Render ImGui menu items for each overlay group in `viewer`.
// If `sync_viewer` is non-null its visibility is kept in sync (split mode).
static inline void overlay_group_menu_items(image_viewer& viewer,
                                             const char* id_prefix,
                                             image_viewer* sync_viewer = nullptr) {
    for (size_t gi = 0; gi < viewer.overlay_group_count(); ++gi) {
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "  %s##%s%zu",
                      viewer.overlay_group_label(gi).c_str(), id_prefix, gi);
        bool vis = viewer.overlay_group_visibility[gi] != 0;
        if (ImGui::MenuItem(lbl, nullptr, &vis)) {
            viewer.overlay_group_visibility[gi] = vis ? 1 : 0;
            if (sync_viewer && gi < sync_viewer->overlay_group_visibility.size())
                sync_viewer->overlay_group_visibility[gi] = vis ? 1 : 0;
        }
    }
}

// Render the interior of a config editor modal: path row (Browse/Reload),
// text editor, and Save button. `id_suffix` keeps ImGui IDs unique.
static inline void config_editor_modal_body(config_tab& tab, const char* id_suffix) {
    char browse_id[48], reload_id[48], save_id[48];
    std::snprintf(browse_id, sizeof(browse_id), "Browse##%s", id_suffix);
    std::snprintf(reload_id, sizeof(reload_id), "Reload##%s", id_suffix);
    std::snprintf(save_id,   sizeof(save_id),   "##save_%s",  id_suffix);

    ImGui::SetNextItemWidth(-180.0f);
    if (ImGui::InputText("##path", &tab.path)) tab.modified = true;
    ImGui::SameLine();
    if (ImGui::Button(browse_id)) {
        nfdchar_t* out = nullptr;
        if (NFD::OpenDialog(out) == NFD_OKAY) {
            tab.path = out;
            NFD::FreePath(out);
            tab.load();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(reload_id)) tab.load();

    const float avail_h = ImGui::GetContentRegionAvail().y
                        - ImGui::GetFrameHeightWithSpacing() - 4;
    if (ImGui::InputTextMultiline("##ed", &tab.text, {-1, avail_h}))
        tab.modified = true;

    const std::string save_label = std::string("Save")
        + (tab.modified ? " *" : "") + save_id;
    if (ImGui::Button(save_label.c_str())) tab.save();
}

// ---------------------------------------------------------------------------
// Returns true if path ends with the given (lower-case) extension including dot.
static bool has_ext(const char* path, const char* ext) {
    const size_t plen = std::strlen(path);
    const size_t elen = std::strlen(ext);
    if (plen < elen) return false;
    for (size_t i = 0; i < elen; ++i) {
        char c = path[plen - elen + i];
        if (c >= 'A' && c <= 'Z') c += 32; // to lower
        if (c != ext[i]) return false;
    }
    return true;
}

static void drop_callback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<app_state*>(glfwGetWindowUserPointer(window));

    // Remote capture mode does not accept file drops.
    if (*app->imode == input_mode::remote_capture) return;
    if (*app->vmode == view_mode::none) return;

    switch (*app->vmode) {
    case view_mode::single:
        for (int i = 0; i < count; ++i) {
            if (has_ext(paths[i], ".json")) {
                load_overlay(paths[i], *app->overlays,
                    [&](auto& g){ app->single_viewer->set_overlay_groups(g); },
                    app->overlay_file, app->status_msg);
            } else {
                app->left_loader->start(paths[i]);
                *app->status_msg = "Loading...";
            }
        }
        return;
    case view_mode::compare: {
        std::vector<const char*> jsonl_files, img_files;
        for (int i = 0; i < count; ++i)
            (has_ext(paths[i], ".json") ? jsonl_files : img_files).push_back(paths[i]);
        if (img_files.size() >= 2) {
            // Two images: first → left, second → right.
            app->left_loader->start(img_files[0]);
            app->right_loader->start(img_files[1]);
        } else if (img_files.size() == 1) {
            // One image: load into the panel the cursor is over.
            int win_w = 1;
            glfwGetWindowSize(window, &win_w, nullptr);
            double cx = 0.0;
            glfwGetCursorPos(window, &cx, nullptr);
            if (cx < win_w * 0.5)
                app->left_loader->start(img_files[0]);
            else
                app->right_loader->start(img_files[0]);
        }
        if (jsonl_files.size() >= 2) {
            load_overlay(jsonl_files[0], *app->left_overlays,
                [&](auto& g){ app->compare->set_left_overlay_groups(g); },
                app->left_overlay_file);
            load_overlay(jsonl_files[1], *app->right_overlays,
                [&](auto& g){ app->compare->set_right_overlay_groups(g); },
                app->right_overlay_file);
        } else if (jsonl_files.size() == 1) {
            int win_w = 1;
            glfwGetWindowSize(window, &win_w, nullptr);
            double cx = 0.0;
            glfwGetCursorPos(window, &cx, nullptr);
            if (cx < win_w * 0.5)
                load_overlay(jsonl_files[0], *app->left_overlays,
                    [&](auto& g){ app->compare->set_left_overlay_groups(g); },
                    app->left_overlay_file);
            else
                load_overlay(jsonl_files[0], *app->right_overlays,
                    [&](auto& g){ app->compare->set_right_overlay_groups(g); },
                    app->right_overlay_file);
        }
        *app->status_msg = "Loading...";
        return;
    }
    default:
        break;
    }
    *app->status_msg = "Loading...";
}

int main(int argc, char** argv) {
    // -------------------------------------------------------------------------
    // Argument parsing (CLI11)
    // -------------------------------------------------------------------------
    CLI::App cli{"VisionStudio - TIFF image viewer"};
    cli.set_version_flag("--version", VS_VERSION_STRING);

    std::string              view_mode_str;
    std::vector<std::string> arg_images;
    std::vector<std::string> arg_overlays;
    bool                     arg_diff    = false;
    float                    arg_amplify = 1.0f;

    auto* img_sub = cli.add_subcommand("img", "Load and view image file(s)");
    img_sub->add_option("--mode", view_mode_str, "View mode: single | compare")
        ->transform(CLI::IsMember({"single", "compare"}, CLI::ignore_case));
    img_sub->add_option("images", arg_images, "Image file(s). One file: single. Two files: compare.")
        ->expected(0, 2);
    img_sub->add_flag("--diff",      arg_diff,    "Enable diff mode on startup");
    img_sub->add_option("--amplify", arg_amplify, "Diff amplification factor (default: 1.0)")
        ->check(CLI::Range(1.0f, 20.0f));
    img_sub->add_option("--overlay", arg_overlays,
                        "JSONL overlay file(s). One file: single. Two files: compare left+right.")
        ->expected(1, 2)
        ->check(CLI::ExistingFile);

    auto* cap_sub = cli.add_subcommand("capture", "Remote capture mode");
    cap_sub->add_option("--mode", view_mode_str, "View mode: single | compare")
        ->transform(CLI::IsMember({"single", "compare"}, CLI::ignore_case));

    CLI11_PARSE(cli, argc, argv);

    const bool input_decided = img_sub->parsed() || cap_sub->parsed();
    input_mode imode = cap_sub->parsed() ? input_mode::remote_capture : input_mode::read_img;
    view_mode  vmode = view_mode_str.empty()        ? view_mode::none
                     : (view_mode_str == "compare") ? view_mode::compare
                                                    : view_mode::single;

    // -------------------------------------------------------------------------
    // GLFW + OpenGL + ImGui init
    // -------------------------------------------------------------------------
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) { fatal_error("glfwInit() failed."); return 1; }

    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // Restore window size from visionstudio.json (defaults to 1280x720).
    int saved_w = 1280, saved_h = 720;
    {
        std::ifstream jf("visionstudio.json");
        if (jf.is_open()) {
            try {
                auto j = nlohmann::json::parse(jf);
                if (j.contains("window")) {
                    const auto& w = j["window"];
                    if (w.contains("width")  && w["width"].is_number_integer())  saved_w = w["width"];
                    if (w.contains("height") && w["height"].is_number_integer()) saved_h = w["height"];
                }
            } catch (...) {}
        }
    }

    GLFWwindow* window = glfwCreateWindow(saved_w, saved_h, "VisionStudio  v" VS_VERSION_STRING, nullptr, nullptr);
    if (!window) {
        const char* err_desc = nullptr;
        int err_code = glfwGetError(&err_desc);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "glfwCreateWindow() failed.\nGLFW error %d: %s",
            err_code, err_desc ? err_desc : "unknown");
        fatal_error(buf);
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, drop_callback);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        fatal_error("gladLoadGLLoader() failed.");
        return 1;
    }

    NFD::Init();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // Disable auto imgui.ini; layout saved in visionstudio.json
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Restore imgui layout from visionstudio.json.
    {
        const std::string ini = capture_config::load_imgui_ini("visionstudio.json");
        if (!ini.empty())
            ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
    }

    // -------------------------------------------------------------------------
    // Application state
    // -------------------------------------------------------------------------
    image_viewer   single_viewer;
    compare_viewer compare;
    image_data     left_image, right_image;

    char left_path_buf[512]  = "";
    char right_path_buf[512] = "";
    std::string status_msg;
    bool        show_pixel_panel      = true;
    bool        show_profile_panel    = false;
    bool        show_overlay_graph    = false;
    // Overlay graph display settings (persisted in visionstudio.json)
    bool   ovg_show_dx    = true;
    bool   ovg_show_dy    = true;
    bool   ovg_show_angle = true;
    bool   ovg_show_fit   = true;
    bool   ovg_show_ref   = false;
    double ovg_ref_a      = 0.0;
    double ovg_ref_b      = 0.0;
    circle_ellipse_tool ce_tool;
    remote_overlay_tool rot;
    bool        show_camera_config    = false;
    bool        show_connect_config   = false;
    bool        show_about            = false;
    bool        show_version          = false;
    std::string error_msg;
    bool        show_log              = false;
    AppLog      app_log;
    bool        show_settings         = false;
    bool        settings_fresh        = false;
    nlohmann::json settings_edit;
    sse_state   cur_sse               = sse_state::disconnected;
    bool        capturing             = false;
    std::vector<cam_info_group>              cam_info;
    std::future<std::vector<cam_info_group>> cam_info_future;

    // Preview texture (MJPEG live preview)
    GLuint preview_tex   = 0;
    int    preview_tex_w = 0;
    int    preview_tex_h = 0;

    // Capture settings
    // capture_mode mirrors vmode for remote_capture; changeable at runtime.
    int  capture_mode      = vmode == view_mode::compare ? 1 : 0;
    bool image_acquisition = true;
    bool live_image        = false;
    bool auto_detect       = true;
    std::string ref_img_path;

    // Editable copies of connection settings (local to UI, applied on Save).
    struct conn_edit {
        char host[128];
        int  port;
        char connect_path[64];
        char start_path[64];
        char stop_path[64];
        char disconnect_path[64];
        char sse_path[64];
        int  timeout_ms;
    };
    auto make_conn_edit = [](const capture_config& c) {
        conn_edit e{};
        std::strncpy(e.host,            c.host.c_str(),            sizeof(e.host) - 1);
        e.port = c.port;
        std::strncpy(e.connect_path,    c.connect_path.c_str(),    sizeof(e.connect_path) - 1);
        std::strncpy(e.start_path,      c.start_path.c_str(),      sizeof(e.start_path) - 1);
        std::strncpy(e.stop_path,       c.stop_path.c_str(),       sizeof(e.stop_path) - 1);
        std::strncpy(e.disconnect_path, c.disconnect_path.c_str(), sizeof(e.disconnect_path) - 1);
        std::strncpy(e.sse_path,        c.sse_path.c_str(),        sizeof(e.sse_path) - 1);
        e.timeout_ms = c.timeout_ms;
        return e;
    };
    config_tab capture_cfg_tab;   // capture_config_file
    config_tab connect_cfg_tab;   // connect_config_file

    // Load overlay graph settings from visionstudio.json
    {
        std::ifstream jf("visionstudio.json");
        if (jf.is_open()) {
            try {
                const auto j = nlohmann::json::parse(jf);
                if (j.contains("overlay_graph") && j["overlay_graph"].is_object()) {
                    const auto& og = j["overlay_graph"];
                    auto gb = [&](const char* k, bool&   v){ if (og.contains(k) && og[k].is_boolean()) v = og[k]; };
                    auto gd = [&](const char* k, double& v){ if (og.contains(k) && og[k].is_number())  v = og[k]; };
                    gb("show_dx",    ovg_show_dx);
                    gb("show_dy",    ovg_show_dy);
                    gb("show_angle", ovg_show_angle);
                    gb("show_fit",   ovg_show_fit);
                    gb("show_ref",   ovg_show_ref);
                    gd("ref_a",      ovg_ref_a);
                    gd("ref_b",      ovg_ref_b);
                }
            } catch (...) {}
        }
    }

    capture_config                cap_cfg  = capture_config::load("visionstudio.json");
    std::optional<capture_client> cap_cli;
    if (imode == input_mode::remote_capture) cap_cli.emplace(cap_cfg);
    conn_edit       conn_buf = make_conn_edit(cap_cfg);

    if (!cap_cfg.capture_config_file.empty()) {
        capture_cfg_tab.path = cap_cfg.capture_config_file;
        capture_cfg_tab.load();
    }
    if (!cap_cfg.connect_config_file.empty()) {
        connect_cfg_tab.path = cap_cfg.connect_config_file;
        connect_cfg_tab.load();
    }

    async_loader           left_loader;
    async_loader           right_loader;
    std::vector<roi_group> overlays;
    std::vector<roi_group> left_overlays;
    std::vector<roi_group> right_overlays;
    std::string            overlay_file;
    std::string            left_overlay_file;
    std::string            right_overlay_file;

    app_state drop_state{&vmode, &imode,
                         &single_viewer, &compare,
                         &left_image, &right_image,
                         &status_msg,
                         &left_loader, &right_loader,
                         &overlays, &left_overlays, &right_overlays,
                         &overlay_file, &left_overlay_file, &right_overlay_file};
    glfwSetWindowUserPointer(window, &drop_state);

    // In remote capture mode launched via CLI, connect automatically.
    if (imode == input_mode::remote_capture)
        cap_cli->connect();

    // Apply diff flags from args (compare mode)
    if (vmode == view_mode::compare) {
        compare.diff_mode    = arg_diff;
        compare.diff_amplify = arg_amplify;
    }

    // Load images specified on command line
    if (!arg_images.empty()) {
        left_loader.start(arg_images[0]);
        if (arg_images.size() >= 2 && vmode == view_mode::compare)
            right_loader.start(arg_images[1]);
        status_msg = "Loading...";
    }

    // Load overlay JSON from CLI args
    if (!arg_overlays.empty()) {
        if (vmode == view_mode::single) {
            load_overlay(arg_overlays[0], overlays,
                [&](auto& g){ single_viewer.set_overlay_groups(g); },
                &overlay_file);
        } else if (vmode == view_mode::compare) {
            load_overlay(arg_overlays[0], left_overlays,
                [&](auto& g){ compare.set_left_overlay_groups(g); },
                &left_overlay_file);
            if (arg_overlays.size() >= 2)
                load_overlay(arg_overlays[1], right_overlays,
                    [&](auto& g){ compare.set_right_overlay_groups(g); },
                    &right_overlay_file);
        }
    }

    // -------------------------------------------------------------------------
    // Main loop
    // -------------------------------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fb_w, fb_h, win_w, win_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glfwGetWindowSize(window, &win_w, &win_h);

        // ----- Mode selection screen -----
        if (vmode == view_mode::none) {
            ImGui::SetNextWindowPos(
                {static_cast<float>(win_w) * 0.5f, static_cast<float>(win_h) * 0.5f},
                ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowBgAlpha(0.92f);
            ImGui::Begin("##mode_select", nullptr,
                ImGuiWindowFlags_NoDecoration    | ImGuiWindowFlags_NoMove        |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoNav);
            if (!input_decided || imode == input_mode::read_img) {
                ImGui::TextDisabled("Image File");
                if (ImGui::Button("Single##img",  {120.0f, 40.0f})) { vmode = view_mode::single;  imode = input_mode::read_img; }
                ImGui::SameLine();
                if (ImGui::Button("Compare##img", {120.0f, 40.0f})) { vmode = view_mode::compare; imode = input_mode::read_img; }
            }
            if (!input_decided || imode == input_mode::remote_capture) {
                if (!input_decided) ImGui::Spacing();
                ImGui::TextDisabled("Remote Capture");
                if (ImGui::Button("Single##cap",  {120.0f, 40.0f})) { vmode = view_mode::single;  imode = input_mode::remote_capture; cap_cli.emplace(cap_cfg); }
                ImGui::SameLine();
                if (ImGui::Button("Compare##cap", {120.0f, 40.0f})) { vmode = view_mode::compare; imode = input_mode::remote_capture; cap_cli.emplace(cap_cfg); }
            }
            ImGui::End();

            ImGui::Render();
            glViewport(0, 0, fb_w, fb_h);
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            continue;
        }

        const bool use_single = (vmode == view_mode::single);

        // ----- Poll async loaders -----
        {
            image_data tmp;
            if (left_loader.poll(tmp)) {
                if (tmp.empty()) {
                    error_msg  = "Failed to open:\n" + left_loader.path;
                    status_msg = "Load failed: " + left_loader.path;
                } else {
                    left_image = std::move(tmp);
                    switch (vmode) {
                    case view_mode::single:
                        single_viewer.load_image(left_image);
                        break;
                    case view_mode::compare:
                        compare.load_left(left_image);
                        compare.left_label = left_loader.path;
                        break;
                    default:
                        break;
                    }
                    rot.set_image_path(left_loader.path);
                    if (!right_loader.active)
                        status_msg = "Loaded: " + left_loader.path;
                }
            }
            if (right_loader.poll(tmp)) {
                if (tmp.empty()) {
                    error_msg  = "Failed to open:\n" + right_loader.path;
                    status_msg = "Load failed: " + right_loader.path;
                } else {
                    right_image = std::move(tmp);
                    compare.load_right(right_image);
                    compare.right_label = right_loader.path;
                    status_msg          = "Loaded";
                }
            }
        }

        // ----- Poll remote overlay result -----
        {
            std::vector<roi_group> remote_groups;
            if (rot.poll_result(remote_groups)) {
                if (use_single) {
                    overlays = remote_groups;
                    single_viewer.set_overlay_groups(std::move(remote_groups));
                } else {
                    left_overlays = remote_groups;
                    compare.set_left_overlay_groups(std::move(remote_groups));
                }
                status_msg = "Remote overlay loaded";
            }
        }

        // ----- Poll capture events (SSE) -----
        if (imode == input_mode::remote_capture) {
            while (auto ev = cap_cli->poll_server_event()) {
                if (std::get_if<evt_connected>(&*ev)) {
                    cur_sse    = sse_state::connected;
                    status_msg = "Connected";
                    app_log.add("INFO", "Connected to server");
                } else if (std::get_if<evt_disconnected>(&*ev)) {
                    cur_sse    = sse_state::disconnected;
                    capturing  = false;
                    status_msg = "Server disconnected";
                    app_log.add("INFO", "Server disconnected");
                } else if (auto* e = std::get_if<evt_error>(&*ev)) {
                    cur_sse    = sse_state::error;
                    status_msg = "Server error: " + e->message;
                    app_log.add("ERROR", ("Server error: " + e->message).c_str());
                } else if (auto* e = std::get_if<evt_capture_done>(&*ev)) {
                    capturing = false;
                    if (capture_mode == 0) {
                        single_viewer.unload_image();
                        left_loader.start(e->path);
                    } else if (capture_mode == 1) {
                        compare.unload_left();
                        compare.unload_right();
                        left_loader.start(e->path);
                    } else {
                        // mode 2: keep left as reference, replace right only
                        compare.unload_right();
                        right_loader.start(e->path);
                    }
                    status_msg = "Capture complete: " + e->path;
                    app_log.add("INFO", ("Capture done: " + e->path).c_str());
                } else if (auto* e = std::get_if<evt_config_updated>(&*ev)) {
                    cap_cfg    = e->cfg;
                    conn_buf   = make_conn_edit(cap_cfg);
                    capture_config::save("visionstudio.json", cap_cfg);
                    status_msg = "Config updated by server";
                    app_log.add("INFO", "Config updated by server");
                } else if (auto* e = std::get_if<evt_camera_info>(&*ev)) {
                    cam_info = std::move(e->groups);
                }
            }

            // ----- Poll camera info future (Refresh button) -----
            if (cam_info_future.valid() &&
                cam_info_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                cam_info = cam_info_future.get();

            // ----- Upload preview frame to GPU -----
            preview_frame pf;
            if (cap_cli->poll_preview_frame(pf)) {
                if (preview_tex == 0 || preview_tex_w != pf.w || preview_tex_h != pf.h) {
                    if (preview_tex == 0) glGenTextures(1, &preview_tex);
                    glBindTexture(GL_TEXTURE_2D, preview_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    const GLint swizzle[] = {GL_RED, GL_RED, GL_RED, GL_ONE};
                    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, pf.w, pf.h, 0,
                                 GL_RED, GL_UNSIGNED_BYTE, pf.pixels.data());
                    preview_tex_w = pf.w;
                    preview_tex_h = pf.h;
                } else {
                    glBindTexture(GL_TEXTURE_2D, preview_tex);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pf.w, pf.h,
                                    GL_RED, GL_UNSIGNED_BYTE, pf.pixels.data());
                }
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }

        // Full-screen host window
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({static_cast<float>(win_w), static_cast<float>(win_h)});
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_MenuBar  |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ----- Menu bar -----
        bool open_file = false;

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (imode == input_mode::read_img)
                    if (ImGui::MenuItem("Open...")) open_file = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (use_single) {
                    ImGui::MenuItem("Show Grid",     nullptr, &single_viewer.show_grid);
                    ImGui::MenuItem("Show Minimap",  nullptr, &single_viewer.show_minimap);
                    ImGui::MenuItem("Show Overlays",  nullptr, &single_viewer.show_overlays);
                    if (single_viewer.show_overlays)
                        overlay_group_menu_items(single_viewer, "ovg");
                    ImGui::MenuItem("Show Tooltip",   nullptr, &single_viewer.show_coordinates);
                    ImGui::MenuItem("Show Crosshair", nullptr, &single_viewer.show_crosshair);
                    if (single_viewer.show_minimap) {
                        ImGui::Separator();
                        ImGui::SetNextItemWidth(160.0f);
                        ImGui::SliderFloat("Minimap Aspect##ms", &single_viewer.minimap_force_aspect, 0.0f, 10.0f, "%.1f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("0 = image aspect  >0 = forced W/H ratio");
                    }
                    if (single_viewer.show_grid) {
                        ImGui::Separator();
                        ImGui::SliderInt("Grid Spacing##s", &single_viewer.grid_spacing, 1, 500);
                    }
                } else {
                    ImGui::MenuItem("Show Grid",     nullptr, &compare.show_grid);
                    ImGui::MenuItem("Show Minimap",  nullptr, &compare.show_minimap);
                    if (compare.show_left_overlays || compare.show_right_overlays)
                        overlay_group_menu_items(compare.left_viewer_ref(), "covg", nullptr);
                    ImGui::MenuItem("Show Tooltip",   nullptr, &compare.show_coordinates);
                    ImGui::MenuItem("Show Crosshair", nullptr, &compare.show_crosshair);
                    ImGui::MenuItem("Sync Views",     nullptr, &compare.sync_views);
                    if (compare.show_minimap) {
                        ImGui::Separator();
                        ImGui::SetNextItemWidth(160.0f);
                        ImGui::SliderFloat("Minimap Aspect##mc", &compare.minimap_force_aspect, 0.0f, 10.0f, "%.1f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("0 = image aspect  >0 = forced W/H ratio");
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Diff Mode", nullptr, &compare.diff_mode))
                        compare.diff_amplify = 1.0f;
                    if (compare.diff_mode)
                        ImGui::SliderFloat("Amplify##d", &compare.diff_amplify, 1.0f, 20.0f);
                    if (compare.show_grid) {
                        ImGui::Separator();
                        ImGui::SliderInt("Grid Spacing##c", &compare.grid_spacing, 1, 500);
                    }
                }
                ImGui::Separator();
                ImGui::MenuItem("Pixel Panel",      nullptr, &show_pixel_panel);
                ImGui::MenuItem("Profile Panel",    nullptr, &show_profile_panel);
                ImGui::MenuItem("Overlay Graph",    nullptr, &show_overlay_graph);
                ImGui::MenuItem("Circle/Ellipse Overlay", nullptr, &ce_tool.visible);
                ImGui::MenuItem("Remote Overlay",         nullptr, &rot.visible);
                ImGui::MenuItem("Log",              nullptr, &show_log);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Settings")) {
                show_settings  = true;
                settings_fresh = true;
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("Version")) show_version = true;
                if (ImGui::MenuItem("About"))   show_about   = true;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // ----- Open file via native dialog -----
        if (open_file) {
            constexpr nfdfilteritem_t kTiffFilter[] = {{"TIFF Image", "tiff,tif"}};
            if (vmode == view_mode::compare) {
                const nfdpathset_t* pathset = nullptr;
                if (NFD::OpenDialogMultiple(pathset, kTiffFilter, 1) == NFD_OKAY) {
                    nfdpathsetsize_t n = 0;
                    NFD::PathSet::Count(pathset, n);
                    if (n >= 1) {
                        nfdchar_t* p = nullptr;
                        NFD::PathSet::GetPath(pathset, 0, p);
                        std::strncpy(left_path_buf, p, sizeof(left_path_buf) - 1);
                        NFD::PathSet::FreePath(p);
                        left_loader.start(left_path_buf);
                    }
                    if (n >= 2) {
                        nfdchar_t* p = nullptr;
                        NFD::PathSet::GetPath(pathset, 1, p);
                        std::strncpy(right_path_buf, p, sizeof(right_path_buf) - 1);
                        NFD::PathSet::FreePath(p);
                        right_loader.start(right_path_buf);
                    }
                    NFD::PathSet::Free(pathset);
                    status_msg = "Loading...";
                }
            } else {
                nfdchar_t* out = nullptr;
                if (NFD::OpenDialog(out, kTiffFilter, 1) == NFD_OKAY) {
                    std::strncpy(left_path_buf, out, sizeof(left_path_buf) - 1);
                    NFD::FreePath(out);
                    left_loader.start(left_path_buf);
                    status_msg = "Loading...";
                }
            }
        }

        // ----- Camera config editor modal -----
        if (show_camera_config) ImGui::OpenPopup("Camera Config##modal");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({700, 540}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Camera Config##modal", &show_camera_config,
                                    ImGuiWindowFlags_NoResize)) {
            config_editor_modal_body(capture_cfg_tab, "cfg");
            ImGui::EndPopup();
        }

        // ----- Connect config editor modal -----
        if (show_connect_config) ImGui::OpenPopup("Connect Config##modal");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({700, 540}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Connect Config##modal", &show_connect_config,
                                    ImGuiWindowFlags_NoResize)) {
            config_editor_modal_body(connect_cfg_tab, "conn");
            ImGui::EndPopup();
        }

        // ----- Version modal -----
        if (show_version) ImGui::OpenPopup("Version##modal");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({320, 0}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Version##modal", &show_version,
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("VisionStudio  v%s", VS_VERSION_STRING);
            ImGui::Separator();
            ImGui::TextDisabled("Build");
            ImGui::Text("  %s", VS_BUILD_TIMESTAMP);
            ImGui::TextDisabled("Commit");
            ImGui::Text("  %s", VS_GIT_HASH);
            ImGui::Spacing();
            if (ImGui::Button("Close", {-1, 0})) { show_version = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // ----- About modal -----
        if (show_about) ImGui::OpenPopup("About VisionStudio##modal");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({620, 520}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("About VisionStudio##modal", &show_about,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::Text("VisionStudio  v%s", VS_VERSION_STRING);
            ImGui::Separator();
            const float text_h = ImGui::GetContentRegionAvail().y
                                - ImGui::GetFrameHeightWithSpacing() - 4;
            // Use a mutable buffer: constexpr data lives in .rdata (read-only) in
            // Release builds, and ImGui may write to the buffer internally.
            static std::string s_license_buf(kThirdPartyLicenses,
                                             sizeof(kThirdPartyLicenses) - 1);
            ImGui::InputTextMultiline("##about_text",
                s_license_buf.data(),
                s_license_buf.size() + 1,
                {-1, text_h},
                ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button("Close")) { show_about = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // ----- Error dialog -----
        if (!error_msg.empty()) ImGui::OpenPopup("Error##err_modal");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Error##err_modal", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.4f, 0.4f, 1.0f});
            ImGui::TextUnformatted(error_msg.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (ImGui::Button("OK", {120, 0})) { error_msg.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // ----- Connecting modal -----
        if (imode == input_mode::remote_capture && cur_sse == sse_state::connecting)
            ImGui::OpenPopup("Connecting##conn_modal");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Connecting##conn_modal", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
            const int    dot_idx = static_cast<int>(ImGui::GetTime() * 2.0) % 4;
            const char*  dots[]  = {"   ", ".  ", ".. ", "..."};
            ImGui::Text("Connecting to camera%s", dots[dot_idx]);
            ImGui::Spacing();
            ImGui::TextDisabled("%s:%d", cap_cfg.host.c_str(), cap_cfg.port);
            ImGui::Spacing();
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 120.0f) * 0.5f +
                                  ImGui::GetCursorPosX());
            if (ImGui::Button("Cancel", {120, 0})) {
                cap_cli->disconnect();
                ImGui::CloseCurrentPopup();
            }
            if (cur_sse != sse_state::connecting)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ----- Settings modal -----
        // On fresh open: populate settings_edit from cap_cfg so all fields are shown.
        if (show_settings && settings_fresh) {
            settings_fresh = false;
            settings_edit = nlohmann::json::object();
            settings_edit["capture"] = {
                {"connect_config_file",  cap_cfg.connect_config_file},
                {"capture_config_file",  cap_cfg.capture_config_file},
                {"basex",                cap_cfg.basex},
                {"targetx",              cap_cfg.targetx},
                {"starty",               cap_cfg.starty},
                {"liney",                cap_cfg.liney},
            };
            settings_edit["capture_client"] = {
                {"host",             cap_cfg.host},
                {"port",             cap_cfg.port},
                {"connect_path",     cap_cfg.connect_path},
                {"start_path",       cap_cfg.start_path},
                {"stop_path",        cap_cfg.stop_path},
                {"disconnect_path",  cap_cfg.disconnect_path},
                {"sse_path",         cap_cfg.sse_path},
                {"preview_path",     cap_cfg.preview_path},
                {"preview_raw_path", cap_cfg.preview_raw_path},
                {"preview_raw",      cap_cfg.preview_raw},
                {"timeout_ms",       cap_cfg.timeout_ms},
            };
            // save_dir is managed separately (folder picker UI)
            settings_edit["save_dir"] = cap_cfg.save_dir;
        }
        if (show_settings) ImGui::OpenPopup("Settings##modal");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({480, 460}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Settings##modal", &show_settings, ImGuiWindowFlags_NoResize)) {
            // Render each JSON section (skip internal keys) dynamically.
            // Sections are CollapsedHeaders; fields are rendered by type.
            static const std::array<std::string, 3> kSkipSections = {"window", "imgui_ini", "save_dir"};
            const float fw = ImGui::GetContentRegionAvail().x;
            for (auto& [section_key, section_val] : settings_edit.items()) {
                bool skip = false;
                for (const auto& s : kSkipSections) if (s == section_key) { skip = true; break; }
                if (skip || !section_val.is_object()) continue;

                if (ImGui::CollapsingHeader(section_key.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Indent(8.0f);
                    for (auto& [key, val] : section_val.items()) {
                        ImGui::TextDisabled("%s", key.c_str());
                        ImGui::SetNextItemWidth(fw - 16.0f);
                        const std::string wid = "##set_" + section_key + "_" + key;
                        if (val.is_string()) {
                            std::string sv = val.get<std::string>();
                            if (ImGui::InputText(wid.c_str(), &sv))
                                val = sv;
                        } else if (val.is_number_integer()) {
                            int iv = val.get<int>();
                            if (ImGui::InputInt(wid.c_str(), &iv, 0))
                                val = iv;
                        } else if (val.is_boolean()) {
                            bool bv = val.get<bool>();
                            const std::string cb_label = key + wid;
                            if (ImGui::Checkbox(cb_label.c_str(), &bv))
                                val = bv;
                        }
                    }
                    ImGui::Unindent(8.0f);
                }
            }

            // ----- Save directory (folder picker) -----
            if (ImGui::CollapsingHeader("Save Directory", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Indent(8.0f);
                std::string save_dir_val = settings_edit.value("save_dir", "");
                const float browse_w = 70.0f;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
                if (ImGui::InputText("##save_dir", &save_dir_val))
                    settings_edit["save_dir"] = save_dir_val;
                ImGui::SameLine();
                if (ImGui::Button("Browse##sd", {browse_w, 0})) {
                    nfdchar_t* out = nullptr;
                    const std::string cur_dir = settings_edit.value("save_dir", "");
                    if (NFD::PickFolder(out, cur_dir.empty() ? nullptr : cur_dir.c_str()) == NFD_OKAY) {
                        settings_edit["save_dir"] = std::string(out);
                        NFD::FreePath(out);
                    }
                }
                ImGui::Unindent(8.0f);
            }

            ImGui::Separator();
            const float btn_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Apply & Save", {btn_w, 0})) {
                // Update cap_cfg from settings_edit sections.
                if (settings_edit.contains("capture_client")) {
                    auto& c = settings_edit["capture_client"];
                    if (c.contains("host")             && c["host"].is_string())              cap_cfg.host             = c["host"].get<std::string>();
                    if (c.contains("port")             && c["port"].is_number_integer())      cap_cfg.port             = c["port"].get<int>();
                    if (c.contains("connect_path")     && c["connect_path"].is_string())      cap_cfg.connect_path     = c["connect_path"].get<std::string>();
                    if (c.contains("start_path")       && c["start_path"].is_string())        cap_cfg.start_path       = c["start_path"].get<std::string>();
                    if (c.contains("stop_path")        && c["stop_path"].is_string())         cap_cfg.stop_path        = c["stop_path"].get<std::string>();
                    if (c.contains("disconnect_path")  && c["disconnect_path"].is_string())   cap_cfg.disconnect_path  = c["disconnect_path"].get<std::string>();
                    if (c.contains("sse_path")         && c["sse_path"].is_string())          cap_cfg.sse_path         = c["sse_path"].get<std::string>();
                    if (c.contains("preview_path")     && c["preview_path"].is_string())      cap_cfg.preview_path     = c["preview_path"].get<std::string>();
                    if (c.contains("preview_raw_path") && c["preview_raw_path"].is_string())  cap_cfg.preview_raw_path = c["preview_raw_path"].get<std::string>();
                    if (c.contains("preview_raw")      && c["preview_raw"].is_boolean())      cap_cfg.preview_raw      = c["preview_raw"].get<bool>();
                    if (c.contains("timeout_ms")       && c["timeout_ms"].is_number_integer()) cap_cfg.timeout_ms      = c["timeout_ms"].get<int>();
                }
                if (settings_edit.contains("capture")) {
                    auto& c = settings_edit["capture"];
                    if (c.contains("connect_config_file") && c["connect_config_file"].is_string()) cap_cfg.connect_config_file = c["connect_config_file"].get<std::string>();
                    if (c.contains("capture_config_file") && c["capture_config_file"].is_string()) cap_cfg.capture_config_file = c["capture_config_file"].get<std::string>();
                    if (c.contains("basex")   && c["basex"].is_number_integer())   cap_cfg.basex   = c["basex"].get<int>();
                    if (c.contains("targetx") && c["targetx"].is_number_integer()) cap_cfg.targetx = c["targetx"].get<int>();
                    if (c.contains("starty")  && c["starty"].is_number_integer())  cap_cfg.starty  = c["starty"].get<int>();
                    if (c.contains("liney")   && c["liney"].is_number_integer())   cap_cfg.liney   = c["liney"].get<int>();
                }
                if (settings_edit.contains("save_dir") && settings_edit["save_dir"].is_string())
                    cap_cfg.save_dir = settings_edit["save_dir"].get<std::string>();
                capture_config::save("visionstudio.json", cap_cfg);
                // Sync conn_buf so the capture panel reflects the new values.
                conn_buf = make_conn_edit(cap_cfg);
                show_settings = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {btn_w, 0})) {
                show_settings = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ----- Toolbar -----
        {
            bool&  show_grid      = use_single ? single_viewer.show_grid        : compare.show_grid;
            bool&  show_minimap   = use_single ? single_viewer.show_minimap     : compare.show_minimap;
            bool&  show_overlays  = use_single ? single_viewer.show_overlays    : compare.show_left_overlays;
            bool&  show_tooltip   = use_single ? single_viewer.show_coordinates : compare.show_coordinates;
            bool&  show_crosshair = use_single ? single_viewer.show_crosshair   : compare.show_crosshair;

            constexpr ImVec4 kOn  = {0.15f, 0.45f, 0.75f, 1.0f};
            constexpr ImVec4 kOnH = {0.25f, 0.55f, 0.85f, 1.0f};
            constexpr ImVec4 kOnA = {0.10f, 0.35f, 0.65f, 1.0f};

            auto toggle_btn = [&](const char* label, bool& flag) {
                const bool on = flag;  // capture before click changes it
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kOn);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOnH);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kOnA);
                }
                if (ImGui::SmallButton(label)) flag = !flag;
                if (on) ImGui::PopStyleColor(3);
                ImGui::SameLine();
            };

            toggle_btn("Grid",      show_grid);
            toggle_btn("Minimap",   show_minimap);
            toggle_btn("Overlays",  show_overlays);
            toggle_btn("Tooltip",   show_tooltip);
            toggle_btn("Crosshair", show_crosshair);
            if (!use_single) {
                toggle_btn("Sync", compare.sync_views);
                const bool diff_on = compare.diff_mode;
                if (diff_on) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kOn);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOnH);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kOnA);
                }
                if (ImGui::SmallButton("Diff")) {
                    compare.diff_mode = !compare.diff_mode;
                    if (compare.diff_mode) compare.diff_amplify = 1.0f;
                }
                if (diff_on) ImGui::PopStyleColor(3);
                ImGui::SameLine();
                ImGui::BeginDisabled(left_image.empty());
                if (ImGui::SmallButton("L\xe2\x86\x92R")) {
                    compare.load_right(left_image);
                    compare.right_label = compare.left_label;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(right_image.empty());
                if (ImGui::SmallButton("R\xe2\x86\x92L")) {
                    compare.load_left(right_image);
                    compare.left_label = compare.right_label;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
            }
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            toggle_btn("Pixel",   show_pixel_panel);
            toggle_btn("Profile", show_profile_panel);
            toggle_btn("OvGraph", show_overlay_graph);
            toggle_btn("Detect",  ce_tool.visible);
            toggle_btn("Remote",  rot.visible);
            ImGui::NewLine();
        }

        // ----- Viewer area -----
        const float status_h          = ImGui::GetFrameHeightWithSpacing();
        const float profile_panel_h   = show_profile_panel  ? 180.0f : 0.0f;
        static float    ovg_panel_h     = 360.0f;  // overlay graph panel height (resizable)
        const float total_avail_h     = ImGui::GetContentRegionAvail().y;
        const float max_ovg_h         = std::max(80.0f, total_avail_h - status_h - profile_panel_h - 80.0f);
        ovg_panel_h                   = std::clamp(ovg_panel_h, 80.0f, max_ovg_h);
        const float overlay_graph_h   = show_overlay_graph  ? ovg_panel_h : 0.0f;
        const float viewer_h          = total_avail_h - status_h
                                        - profile_panel_h - overlay_graph_h;

        static float    panel_w         = 240.0f;  // right pixel panel (resizable)
        static float    capture_panel_w = 180.0f;  // left capture control panel (resizable)
        const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
        const float avail_x   = ImGui::GetContentRegionAvail().x;

        const float left_w  = (imode == input_mode::remote_capture) ? capture_panel_w + spacing_x : 0.0f;
        const float right_w = show_pixel_panel ? panel_w + spacing_x : 0.0f;
        const float viewer_w = (left_w > 0.0f || right_w > 0.0f)
            ? avail_x - left_w - right_w
            : 0.0f;

        // ----- Left capture control panel -----
        if (imode == input_mode::remote_capture) {
            if (ImGui::BeginChild("##capture_ctrl", {capture_panel_w, viewer_h},
                                  ImGuiChildFlags_Borders)) {
                // SSE status indicator
                const char* sse_label = "";
                ImVec4      sse_col   = {1, 1, 1, 1};
                switch (cur_sse) {
                case sse_state::disconnected:
                    sse_label = "Disconnected"; sse_col = {0.6f, 0.6f, 0.6f, 1}; break;
                case sse_state::connecting:
                    sse_label = "Connecting..."; sse_col = {1, 0.8f, 0, 1};      break;
                case sse_state::connected:
                    sse_label = "Connected";    sse_col = {0.2f, 1, 0.4f, 1};   break;
                case sse_state::error:
                    sse_label = "Error";        sse_col = {1, 0.3f, 0.3f, 1};   break;
                }
                ImGui::TextColored(sse_col, "SSE: %s", sse_label);
                ImGui::Separator();

                // Connection settings (collapsible)
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
                const bool conn_open = ImGui::CollapsingHeader("Connect Settings");
                ImGui::PopStyleColor(3);
                if (conn_open) {
                    ImGui::BeginDisabled(cur_sse == sse_state::connected);
                    bool conn_changed = false;
                    const float label_col_w = ImGui::CalcTextSize("Timeout(ms)").x
                                           + ImGui::GetStyle().ItemSpacing.x;
                    auto labeled = [&](const char* label, auto fn) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", label);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemWidth(-1);
                        if (fn()) conn_changed = true;
                    };
                    constexpr ImGuiTableFlags kTblFlags = ImGuiTableFlags_None;
                    if (ImGui::BeginTable("##conn_host", 2, kTblFlags)) {
                        ImGui::TableSetupColumn("##lbl1", ImGuiTableColumnFlags_WidthFixed, label_col_w);
                        ImGui::TableSetupColumn("##val1", ImGuiTableColumnFlags_WidthStretch);
                        labeled("Host", [&]{ return ImGui::InputText("##host", conn_buf.host, sizeof(conn_buf.host)); });
                        labeled("Port", [&]{ return ImGui::InputInt ("##port", &conn_buf.port, 0); });
                        ImGui::EndTable();
                    }
                    ImGui::Separator();
                    if (ImGui::BeginTable("##conn_paths", 2, kTblFlags)) {
                        ImGui::TableSetupColumn("##lbl2", ImGuiTableColumnFlags_WidthFixed, label_col_w);
                        ImGui::TableSetupColumn("##val2", ImGuiTableColumnFlags_WidthStretch);
                        labeled("Connect",    [&]{ return ImGui::InputText("##conn_path",  conn_buf.connect_path,    sizeof(conn_buf.connect_path)); });
                        labeled("Start",      [&]{ return ImGui::InputText("##start_path", conn_buf.start_path,      sizeof(conn_buf.start_path)); });
                        labeled("Stop",       [&]{ return ImGui::InputText("##stop_path",  conn_buf.stop_path,       sizeof(conn_buf.stop_path)); });
                        labeled("Disconnect", [&]{ return ImGui::InputText("##disc_path",  conn_buf.disconnect_path, sizeof(conn_buf.disconnect_path)); });
                        labeled("SSE",        [&]{ return ImGui::InputText("##sse_path",   conn_buf.sse_path,        sizeof(conn_buf.sse_path)); });
                        labeled("Timeout(ms)",[&]{ return ImGui::InputInt ("##timeout",    &conn_buf.timeout_ms,     0); });
                        ImGui::EndTable();
                    }
                    if (conn_changed) {
                        cap_cfg.host            = conn_buf.host;
                        cap_cfg.port            = conn_buf.port;
                        cap_cfg.connect_path    = conn_buf.connect_path;
                        cap_cfg.start_path      = conn_buf.start_path;
                        cap_cfg.stop_path       = conn_buf.stop_path;
                        cap_cfg.disconnect_path = conn_buf.disconnect_path;
                        cap_cfg.sse_path        = conn_buf.sse_path;
                        cap_cfg.timeout_ms      = conn_buf.timeout_ms;
                        capture_config::save("visionstudio.json", cap_cfg);
                    }
                    if (!connect_cfg_tab.path.empty()) {
                        ImGui::Separator();
                        ImGui::TextDisabled("Connect Config File");
                        const auto& p   = connect_cfg_tab.path;
                        const auto  pos = p.find_last_of("/\\");
                        const std::string fname = (pos == std::string::npos) ? p : p.substr(pos + 1);
                        if (ImGui::Button(fname.c_str(), {-1, 0}))
                            show_connect_config = true;
                    }
                    ImGui::EndDisabled();
                }
                ImGui::Separator();

                // Connection buttons (blue)
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.15f, 0.45f, 0.75f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.22f, 0.58f, 0.90f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.10f, 0.32f, 0.55f, 1.0f});
                ImGui::BeginDisabled(cur_sse != sse_state::disconnected &&
                                     cur_sse != sse_state::error);
                if (ImGui::Button("Connect", {-1, 0})) {
                    cap_cli->connect();
                    status_msg = "Connecting...";
                }
                ImGui::EndDisabled();

                ImGui::BeginDisabled(cur_sse != sse_state::connected);
                if (ImGui::Button("Disconnect", {-1, 0})) {
                    cap_cli->disconnect();
                    capturing  = false;
                    status_msg = "Disconnecting...";
                }
                ImGui::EndDisabled();
                ImGui::PopStyleColor(3);
                ImGui::Separator();

                // Capture Settings (collapsible)
                // In debug builds, always accessible; in release, requires connection.
#ifndef NDEBUG
                constexpr bool cap_settings_disabled = false;
#else
                const bool cap_settings_disabled = cur_sse != sse_state::connected;
#endif
                ImGui::BeginDisabled(cap_settings_disabled);
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
                const bool cap_settings_open = ImGui::CollapsingHeader("Capture Settings");
                ImGui::PopStyleColor(3);
                if (cap_settings_open) {
                    constexpr const char* kCaptureModes[] = {"Single", "Compare", "Compare (keep left)"};
                    ImGui::TextDisabled("View Mode");
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    const int prev_cap_mode = capture_mode;
                    if (ImGui::Combo("##cap_mode", &capture_mode, kCaptureModes, 3)) {
                        if (prev_cap_mode == 1 && capture_mode == 0) {
                            ref_img_path.clear();
                            compare.unload_left();
                        }
                        vmode = capture_mode == 0 ? view_mode::single : view_mode::compare;
                    }
                    ImGui::Separator();

                    if (ImGui::BeginTable("##sw_table", 2, ImGuiTableFlags_None)) {
                        const float sw_lbl_w = ImGui::CalcTextSize("Image Acquisition").x
                                             + ImGui::GetStyle().ItemSpacing.x;
                        ImGui::TableSetupColumn("##sw_lbl", ImGuiTableColumnFlags_WidthFixed, sw_lbl_w);
                        ImGui::TableSetupColumn("##sw_val", ImGuiTableColumnFlags_WidthFixed);
                        auto sw_row = [&](const char* label, const char* id, bool* val) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextDisabled("%s", label);
                            ImGui::TableSetColumnIndex(1);
                            toggle_switch(id, val);
                        };
                        sw_row("Image Acquisition", "##acq",  &image_acquisition);
                        sw_row("Live Image",        "##live", &live_image);
                        sw_row("Auto Detect",       "##ad",   &auto_detect);
                        ImGui::EndTable();
                    }

                    ImGui::BeginDisabled(vmode != view_mode::compare);
                    ImGui::TextDisabled("Ref Img");
                    {
                        const auto pos = ref_img_path.find_last_of("/\\");
                        const std::string fname = ref_img_path.empty()
                            ? "(none)"
                            : (pos == std::string::npos ? ref_img_path : ref_img_path.substr(pos + 1));
                        if (ImGui::Button(fname.c_str(), {-1, 0})) {
                            constexpr nfdfilteritem_t kTiffFilter[] = {{"TIFF Image", "tiff,tif"}};
                            nfdchar_t* out = nullptr;
                            if (NFD::OpenDialog(out, kTiffFilter, 1) == NFD_OKAY) {
                                ref_img_path = out;
                                NFD::FreePath(out);
                                left_loader.start(ref_img_path);
                                status_msg = "Loading...";
                            }
                        }
                    }
                    ImGui::EndDisabled();

                    // Guide lines
                    ImGui::Separator();
                    ImGui::TextDisabled("Guide Lines  (-1 = off)");
                    if (ImGui::BeginTable("##guide_table", 2, ImGuiTableFlags_None)) {
                        const float gl_lbl_w = ImGui::CalcTextSize("targetx").x
                                             + ImGui::GetStyle().ItemSpacing.x;
                        ImGui::TableSetupColumn("##gl_lbl", ImGuiTableColumnFlags_WidthFixed, gl_lbl_w);
                        ImGui::TableSetupColumn("##gl_val", ImGuiTableColumnFlags_WidthStretch);
                        auto gl_row = [&](const char* label, const char* id, int* val) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextDisabled("%s", label);
                            ImGui::TableSetColumnIndex(1);
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::InputInt(id, val, 0))
                                capture_config::save("visionstudio.json", cap_cfg);
                        };
                        gl_row("basex",   "##basex",   &cap_cfg.basex);
                        gl_row("targetx", "##targetx", &cap_cfg.targetx);
                        gl_row("starty",  "##starty",  &cap_cfg.starty);
                        gl_row("liney",   "##liney",   &cap_cfg.liney);
                        ImGui::EndTable();
                    }

                    // Capture config files: path + browse + edit modal
                    ImGui::Separator();
                    ImGui::TextDisabled("Capture Config Files");
                    const float fw = ImGui::GetContentRegionAvail().x;
                    if (!capture_cfg_tab.path.empty()) {
                        const auto& fpath = capture_cfg_tab.path;
                        const auto  pos   = fpath.find_last_of("/\\");
                        const std::string fname = (pos == std::string::npos) ? fpath : fpath.substr(pos + 1);
                        if (ImGui::Button(fname.c_str(), {-1, 0}))
                            show_camera_config = true;
                    }
                }
                ImGui::EndDisabled();
                ImGui::Separator();

                // Capture buttons
                ImGui::BeginDisabled(cur_sse != sse_state::connected || capturing);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.18f, 0.55f, 0.18f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.25f, 0.70f, 0.25f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.12f, 0.40f, 0.12f, 1.0f});
                if (ImGui::Button("Start Capture", {-1, 0})) {
                    cap_cli->start_capture();
                    capturing  = true;
                    status_msg = "Capture started";
                }
                ImGui::PopStyleColor(3);
                ImGui::EndDisabled();

                ImGui::BeginDisabled(cur_sse != sse_state::connected || !capturing);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.60f, 0.15f, 0.15f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.78f, 0.20f, 0.20f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.45f, 0.10f, 0.10f, 1.0f});
                if (ImGui::Button("Stop Capture", {-1, 0})) {
                    cap_cli->stop_capture();
                    capturing  = false;
                    status_msg = "Capture stopped";
                }
                ImGui::PopStyleColor(3);
                ImGui::EndDisabled();

                // Preview
                ImGui::Separator();
                const bool preview_on = cap_cli->is_preview_active();
                ImGui::BeginDisabled(preview_on);
                if (ImGui::Checkbox("Raw", &cap_cfg.preview_raw)) {
                    capture_config::save("visionstudio.json", cap_cfg);
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(cur_sse != sse_state::connected);
                if (!preview_on) {
                    if (ImGui::Button("Start Preview", {-1, 0})) {
                        if (preview_tex != 0) {
                            glDeleteTextures(1, &preview_tex);
                            preview_tex   = 0;
                            preview_tex_w = 0;
                            preview_tex_h = 0;
                        }
                        cap_cli->start_preview();
                        status_msg = "Preview started";
                    }
                } else {
                    if (ImGui::Button("Stop Preview", {-1, 0})) {
                        cap_cli->stop_preview();
                        status_msg = "Preview stopped";
                    }
                }
                ImGui::EndDisabled();
            }

            // ----- Camera Info -----
            ImGui::Separator();
            static std::string cam_edit_key;
            static char        cam_edit_buf[256] = {};
            static bool        cam_edit_focus    = false;

            ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
            const bool cam_info_open = ImGui::CollapsingHeader("Camera Info");
            ImGui::PopStyleColor(3);
            if (cam_info_open) {
                const bool fetching = cam_info_future.valid() &&
                    cam_info_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
                ImGui::BeginDisabled(fetching || cur_sse != sse_state::connected);
                if (ImGui::SmallButton(fetching ? "Fetching..." : "Refresh Info")) {
                    cam_info_future = std::async(std::launch::async,
                        [&cap_cli]{ return cap_cli->fetch_camera_info(); });
                }
                ImGui::EndDisabled();

            for (auto& g : cam_info) {
                if (ImGui::CollapsingHeader(g.label.c_str())) {
                    if (ImGui::BeginTable(g.label.c_str(), 2,
                            ImGuiTableFlags_SizingFixedFit |
                            ImGuiTableFlags_RowBg          |
                            ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 0.5f);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                        for (auto& p : g.params) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            const float row_top = ImGui::GetCursorScreenPos().y;
                            const ImVec2 mouse   = ImGui::GetMousePos();
                            const bool row_hov   = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                                                   mouse.y >= row_top &&
                                                   mouse.y <  row_top + ImGui::GetTextLineHeightWithSpacing();
                            ImGui::TextDisabled("%s", p.name.c_str());
                            ImGui::TableSetColumnIndex(1);

                            if (p.rw_type == cam_param_rw::writeonly) {
                                ImGui::TextDisabled("---");
                            } else if (p.rw_type == cam_param_rw::readonly) {
                                const std::string disp = p.unit.empty()
                                    ? p.value : p.value + " " + p.unit;
                                ImGui::TextDisabled("%s", disp.c_str());
                            } else {
                                // readwrite: double-click to edit
                                const std::string key      = g.label + "/" + p.name;
                                const std::string popup_id = "##ep_" + key;

                                if (cam_edit_key == key) {
                                    if (p.type == cam_param_type::bool_ ||
                                        p.type == cam_param_type::enum_) {
                                        // Popup with selectable options
                                        if (ImGui::BeginPopup(popup_id.c_str())) {
                                            for (const auto& opt : p.options) {
                                                if (ImGui::Selectable(opt.c_str(), opt == p.value)) {
                                                    p.value = opt;
                                                    cam_edit_key.clear();
                                                    ImGui::CloseCurrentPopup();
                                                    cap_cli->update_param(p.name, p.value);
                                                }
                                            }
                                            ImGui::EndPopup();
                                        } else {
                                            cam_edit_key.clear();
                                        }
                                        // Show current value in table cell below popup
                                        const std::string disp = p.unit.empty()
                                            ? p.value : p.value + " " + p.unit;
                                        ImGui::TextUnformatted(disp.c_str());
                                    } else {
                                        // InputText for int_ / float_ / string_
                                        if (cam_edit_focus) {
                                            ImGui::SetKeyboardFocusHere();
                                            cam_edit_focus = false;
                                        }
                                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                                        const bool enter = ImGui::InputText(
                                            ("##camedit_" + p.name).c_str(),
                                            cam_edit_buf, sizeof(cam_edit_buf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
                                        if (enter || ImGui::IsItemDeactivated()) {
                                            const std::string raw = cam_edit_buf;
                                            if (p.type == cam_param_type::int_) {
                                                try {
                                                    int v = std::stoi(raw);
                                                    if (!p.min.empty())
                                                        v = std::max(v, std::stoi(p.min));
                                                    if (!p.max.empty())
                                                        v = std::min(v, std::stoi(p.max));
                                                    p.value = std::to_string(v);
                                                } catch (...) {}
                                            } else if (p.type == cam_param_type::float_) {
                                                try {
                                                    float v = std::stof(raw);
                                                    if (!p.min.empty())
                                                        v = std::max(v, std::stof(p.min));
                                                    if (!p.max.empty())
                                                        v = std::min(v, std::stof(p.max));
                                                    char fb[64];
                                                    std::snprintf(fb, sizeof(fb), "%g",
                                                                  static_cast<double>(v));
                                                    p.value = fb;
                                                } catch (...) {}
                                            } else {
                                                p.value = raw;
                                            }
                                            cam_edit_key.clear();
                                            cap_cli->update_param(p.name, p.value);
                                        }
                                    }
                                } else {
                                    // Display mode — detect double-click to enter edit
                                    const std::string disp = p.unit.empty()
                                        ? p.value : p.value + " " + p.unit;
                                    ImGui::TextUnformatted(disp.c_str());
                                    if (row_hov &&
                                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                        cam_edit_key = key;
                                        if (p.type == cam_param_type::bool_ ||
                                            p.type == cam_param_type::enum_) {
                                            ImGui::OpenPopup(popup_id.c_str());
                                        } else {
                                            std::strncpy(cam_edit_buf, p.value.c_str(),
                                                         sizeof(cam_edit_buf) - 1);
                                            cam_edit_buf[sizeof(cam_edit_buf) - 1] = '\0';
                                            cam_edit_focus = true;
                                        }
                                    }
                                }
                            }
                            if (row_hov && cam_edit_key.empty() &&
                                (!p.description.empty() || !p.min.empty() || !p.initial.empty())) {
                                ImGui::BeginTooltip();
                                if (!p.description.empty()) ImGui::TextUnformatted(p.description.c_str());
                                if (!p.min.empty() || !p.max.empty())
                                    ImGui::Text("Range: %s - %s", p.min.c_str(), p.max.c_str());
                                if (!p.initial.empty())
                                    ImGui::Text("Default: %s %s", p.initial.c_str(), p.unit.c_str());
                                ImGui::EndTooltip();
                            }
                        }
                        ImGui::EndTable();
                    }
                }
            } // end group loop
            } // end CollapsingHeader "Camera Info"

            ImGui::EndChild();
            ImGui::SameLine();
        }

        const ImVec2 viewer_origin = ImGui::GetCursorScreenPos();

        if (imode == input_mode::remote_capture && use_single && preview_tex != 0) {
            // Full-viewer live preview (single mode)
            const float aspect = static_cast<float>(preview_tex_w) / static_cast<float>(preview_tex_h);
            float dw = viewer_w, dh = viewer_w / aspect;
            if (dh > viewer_h) { dh = viewer_h; dw = viewer_h * aspect; }
            const auto orig = ImGui::GetCursorPos();
            ImGui::SetCursorPos({orig.x + (viewer_w - dw) * 0.5f,
                                  orig.y + (viewer_h - dh) * 0.5f});
            ImGui::Image(static_cast<ImTextureID>(preview_tex), {dw, dh});
        } else if (use_single) {
            single_viewer.render("single_canvas", viewer_w, viewer_h);
            if (ce_tool.visible) {
                const view_state& vs = single_viewer.get_view_state();
                ce_tool.render_overlay(ImGui::GetWindowDrawList(),
                                       viewer_origin, {viewer_w, viewer_h},
                                       vs.zoom, vs.pan_x, vs.pan_y);
            }
        } else {
            compare.render(viewer_w, viewer_h);
            if (ce_tool.visible) {
                const view_state& lvs = compare.get_view_state();
                const float spacing   = ImGui::GetStyle().ItemSpacing.x;
                const float half_w    = std::floor((viewer_w - spacing) * 0.5f);
                ce_tool.render_overlay(ImGui::GetWindowDrawList(),
                                       viewer_origin, {half_w, viewer_h},
                                       lvs.zoom, lvs.pan_x, lvs.pan_y);
            }
        }

        // For split/compare capture: overlay live preview on the right panel
        if (imode == input_mode::remote_capture && !use_single && preview_tex != 0) {
            const float spacing  = ImGui::GetStyle().ItemSpacing.x;
            const float half_w   = std::floor((viewer_w - spacing) * 0.5f);
            const ImVec2 rmin    = {viewer_origin.x + half_w + spacing, viewer_origin.y};
            const ImVec2 rmax    = {viewer_origin.x + viewer_w,         viewer_origin.y + viewer_h};
            const float  rw      = rmax.x - rmin.x;
            const float  rh      = rmax.y - rmin.y;
            const float  aspect  = static_cast<float>(preview_tex_w) / static_cast<float>(preview_tex_h);
            float dw = rw, dh = rw / aspect;
            if (dh > rh) { dh = rh; dw = rh * aspect; }
            const ImVec2 imin = {rmin.x + (rw - dw) * 0.5f, rmin.y + (rh - dh) * 0.5f};
            const ImVec2 imax = {imin.x + dw,                imin.y + dh};
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(rmin, rmax, IM_COL32(20, 20, 20, 255));
            dl->AddImage(static_cast<ImTextureID>(preview_tex), imin, imax);
        }

        // ----- Guide lines overlay -----
        {
            const bool has_guide = cap_cfg.basex >= 0 || cap_cfg.targetx >= 0
                                || cap_cfg.starty >= 0 || cap_cfg.liney   >= 0;
            if (has_guide && imode == input_mode::remote_capture && !cap_cli->is_preview_active()) {
                auto* dl = ImGui::GetWindowDrawList();
                const ImU32 vcol = IM_COL32(255, 80,  80,  200);
                const ImU32 hcol = IM_COL32( 80, 200, 255, 200);

                auto draw_guides = [&](ImVec2 img_orig, float img_scale,
                                       float cx0, float cy0, float cx1, float cy1) {
                    dl->PushClipRect({cx0, cy0}, {cx1, cy1}, true);
                    if (cap_cfg.basex   >= 0) dl->AddLine({img_orig.x + cap_cfg.basex   * img_scale, cy0}, {img_orig.x + cap_cfg.basex   * img_scale, cy1}, vcol, 1.5f);
                    if (cap_cfg.targetx >= 0) dl->AddLine({img_orig.x + cap_cfg.targetx * img_scale, cy0}, {img_orig.x + cap_cfg.targetx * img_scale, cy1}, vcol, 1.5f);
                    if (cap_cfg.starty  >= 0) dl->AddLine({cx0, img_orig.y + cap_cfg.starty  * img_scale}, {cx1, img_orig.y + cap_cfg.starty  * img_scale}, hcol, 1.5f);
                    if (cap_cfg.liney   >= 0) dl->AddLine({cx0, img_orig.y + cap_cfg.liney   * img_scale}, {cx1, img_orig.y + cap_cfg.liney   * img_scale}, hcol, 1.5f);
                    dl->PopClipRect();
                };

                if (use_single) {
                    if (preview_tex == 0 && single_viewer.has_image()) {
                        const view_state& vs = single_viewer.get_view_state();
                        const float ox = single_viewer.display_offset_x() * vs.zoom;
                        const float oy = single_viewer.display_offset_y() * vs.zoom;
                        draw_guides({viewer_origin.x + vs.pan_x - ox, viewer_origin.y + vs.pan_y - oy},
                                    vs.zoom,
                                    viewer_origin.x, viewer_origin.y,
                                    viewer_origin.x + viewer_w, viewer_origin.y + viewer_h);
                    }
                } else if (compare.left_viewer_ref().has_image()) {
                    const float spacing    = ImGui::GetStyle().ItemSpacing.x;
                    const float half_w     = std::floor((viewer_w - spacing) * 0.5f);
                    const float header_h   = compare.get_header_height();
                    const float canvas_top = viewer_origin.y + header_h;
                    const view_state& vs   = compare.get_view_state();

                    const float lox = compare.left_viewer_ref().display_offset_x()  * vs.zoom;
                    const float loy = compare.left_viewer_ref().display_offset_y()  * vs.zoom;
                    draw_guides({viewer_origin.x + vs.pan_x - lox, canvas_top + vs.pan_y - loy},
                                vs.zoom,
                                viewer_origin.x, canvas_top,
                                viewer_origin.x + half_w, viewer_origin.y + viewer_h);

                    const float rox = compare.right_viewer_ref().display_offset_x() * vs.zoom;
                    const float roy = compare.right_viewer_ref().display_offset_y()  * vs.zoom;
                    draw_guides({viewer_origin.x + half_w + spacing + vs.pan_x - rox, canvas_top + vs.pan_y - roy},
                                vs.zoom,
                                viewer_origin.x + half_w + spacing, canvas_top,
                                viewer_origin.x + viewer_w,         viewer_origin.y + viewer_h);
                }
            }
        }

        // ----- Shared helpers for profile panels -----
        auto draw_rgba = [](const char* id, const std::array<uint8_t, 4>& rgba,
                            PixelFormat fmt = PixelFormat::rgba) {
            const ImVec4 cv{rgba[0]/255.f, rgba[1]/255.f, rgba[2]/255.f, rgba[3]/255.f};
            ImGui::ColorButton(id, cv,
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, {16, 16});
            ImGui::SameLine();
            if (fmt == PixelFormat::gray)
                ImGui::Text("Gray: %3d", rgba[0]);
            else
                ImGui::Text("R:%3d  G:%3d  B:%3d  A:%3d",
                            rgba[0], rgba[1], rgba[2], rgba[3]);
        };

        struct series_entry { const image_data* img; ImU32 color; int cursor; };

        // draw_profile: renders a luminance profile using ImPlot.
        //   vis_min/vis_max : when >= 0, the visible-range data is stretched across
        //                     the full X axis and overlaid in yellow so you can compare
        //                     detail against the overall shape.
        auto draw_profile = [&](const char* plot_id, bool is_x, int fixed,
                                std::initializer_list<series_entry> series,
                                float gw, float gh,
                                int vis_min = -1, int vis_max = -1) {
            int total = 0;
            for (const auto& s : series) {
                if (s.img && !s.img->empty()) {
                    total = is_x ? s.img->width : s.img->height;
                    break;
                }
            }

            const bool has_vis = vis_min >= 0 && vis_max > vis_min;

            const ImPlotFlags plot_flags =
                ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText;
            constexpr ImPlotAxisFlags kAxFlags =
                ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels;

            if (ImPlot::BeginPlot(plot_id, {gw, gh}, plot_flags)) {
                ImPlot::SetupAxes(nullptr, nullptr, kAxFlags, kAxFlags);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, total > 1 ? total - 1 : 1, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 255, ImGuiCond_Always);

                int sidx = 0;
                for (const auto& s : series) {
                    if (!s.img || s.img->empty() || fixed < 0 || total <= 0) { ++sidx; continue; }
                    const int n = is_x ? s.img->width : s.img->height;
                    std::vector<float> ys(n);
                    for (int i = 0; i < n; ++i) {
                        const auto px = is_x ? s.img->pixel_at(i, fixed)
                                             : s.img->pixel_at(fixed, i);
                        ys[i] = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
                    }

                    // Full-range line.
                    char lbl[16]; snprintf(lbl, sizeof(lbl), "##lum%d", sidx);
                    ImPlot::SetNextLineStyle(ImGui::ColorConvertU32ToFloat4(s.color), 1.0f);
                    ImPlot::PlotLine(lbl, ys.data(), n);

                    // Visible-range stretched overlay: extract vis slice and
                    // scale its x positions to span 0..total-1 (same axis).
                    if (has_vis) {
                        const int v0 = std::max(0, vis_min);
                        const int v1 = std::min(n - 1, vis_max);
                        const int m  = v1 - v0 + 1;
                        if (m > 1) {
                            std::vector<float> vis_ys(ys.begin() + v0, ys.begin() + v1 + 1);
                            // xscale maps m points to span [0 .. total-1].
                            const double xscale = static_cast<double>(total - 1) / (m - 1);
                            char vlbl[16]; snprintf(vlbl, sizeof(vlbl), "##vis%d", sidx);
                            ImPlot::SetNextLineStyle(
                                ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 220, 60, 200)), 1.5f);
                            ImPlot::PlotLine(vlbl, vis_ys.data(), m, xscale, 0.0);
                        }
                    }

                    // Cursor: vertical red line at hover position.
                    if (s.cursor >= 0 && s.cursor < n) {
                        const double cx[2] = {(double)s.cursor, (double)s.cursor};
                        const double cy[2] = {0.0, 255.0};
                        char clbl[16]; snprintf(clbl, sizeof(clbl), "##cur%d", sidx);
                        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.31f, 0.31f, 0.9f), 1.5f);
                        ImPlot::PlotLine(clbl, cx, cy, 2);
                    }
                    ++sidx;
                }
                ImPlot::EndPlot();
            }
        };

        // ----- Resize handles -----
        if (imode == input_mode::remote_capture) {
            const float handle_x = viewer_origin.x - spacing_x * 0.5f - 2.0f;
            ImGui::SetCursorScreenPos({handle_x, viewer_origin.y});
            ImGui::InvisibleButton("##capture_resize", {4.0f, viewer_h});
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive())
                capture_panel_w = std::clamp(capture_panel_w + ImGui::GetIO().MouseDelta.x, 120.0f, 400.0f);
        }
        if (show_pixel_panel) {
            const float handle_x = viewer_origin.x + viewer_w + spacing_x * 0.5f - 2.0f;
            ImGui::SetCursorScreenPos({handle_x, viewer_origin.y});
            ImGui::InvisibleButton("##panel_resize", {4.0f, viewer_h});
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive())
                panel_w = std::clamp(panel_w - ImGui::GetIO().MouseDelta.x, 160.0f, 600.0f);
        }
        // ----- Right panel (tabbed) -----
        if (show_pixel_panel) {
            ImGui::SetCursorScreenPos({viewer_origin.x + viewer_w + spacing_x, viewer_origin.y});
            ImGui::BeginChild("##pixel_panel", {panel_w, viewer_h}, ImGuiChildFlags_Borders);

            // ---- Info section (always visible) ----
            if (use_single) {
                const auto& hi = single_viewer.get_hover_info();
                if (!hi.valid) {
                    ImGui::TextDisabled("--");
                } else {
                    ImGui::Text("pos  : (%d, %d)", hi.img_x, hi.img_y);
                    ImGui::Text("zoom : %.2fx", static_cast<double>(hi.zoom));
                    ImGui::Separator();
                    draw_rgba("##swatch", hi.rgba, single_viewer.get_image_data().format);
                }
            } else {
                const auto& hi = compare.get_hover_info();
                if (!hi.valid) {
                    ImGui::TextDisabled("--");
                } else {
                    ImGui::Text("pos  : (%d, %d)", hi.img_x, hi.img_y);
                    ImGui::Text("zoom : %.2fx", static_cast<double>(hi.zoom));
                    ImGui::Separator();
                    ImGui::TextDisabled("Left");
                    draw_rgba("##lswatch", hi.left_rgba,
                              compare.left_viewer_ref().get_image_data().format);
                    ImGui::Spacing();
                    ImGui::TextDisabled(compare.diff_mode ? "Diff" : "Right");
                    draw_rgba("##rswatch", hi.right_rgba,
                              compare.right_viewer_ref().get_image_data().format);
                }
            }

            // ----- Overlay file selector -----
            if (imode == input_mode::read_img) {
                ImGui::Separator();
                ImGui::TextDisabled("Overlay");

                const float load_w = 45.0f;
                const float path_w = ImGui::GetContentRegionAvail().x
                                     - load_w - ImGui::GetStyle().ItemSpacing.x;

                if (vmode == view_mode::compare) {
                    ImGui::TextDisabled("L");
                    ImGui::SetNextItemWidth(path_w);
                    ImGui::InputText("##ov_path_l", &left_overlay_file, ImGuiInputTextFlags_ReadOnly);
                    ImGui::SameLine();
                    if (ImGui::Button("Load##ovl", {load_w, 0}))
                        load_overlay(left_overlay_file, left_overlays,
                            [&](auto& g){ compare.set_left_overlay_groups(g); },
                            nullptr, &status_msg, "Overlay L loaded: ");
                    overlay_group_checkboxes(compare.left_viewer_ref(), "ovgl");
                    ImGui::TextDisabled("R");
                    ImGui::SetNextItemWidth(path_w);
                    ImGui::InputText("##ov_path_r", &right_overlay_file, ImGuiInputTextFlags_ReadOnly);
                    ImGui::SameLine();
                    if (ImGui::Button("Load##ovr", {load_w, 0}))
                        load_overlay(right_overlay_file, right_overlays,
                            [&](auto& g){ compare.set_right_overlay_groups(g); },
                            nullptr, &status_msg, "Overlay R loaded: ");
                    overlay_group_checkboxes(compare.right_viewer_ref(), "ovgr");
                } else {
                    ImGui::SetNextItemWidth(path_w);
                    ImGui::InputText("##ov_path", &overlay_file, ImGuiInputTextFlags_ReadOnly);
                    ImGui::SameLine();
                    if (ImGui::Button("Load##ov", {load_w, 0})) {
                        if (use_single)
                            load_overlay(overlay_file, overlays,
                                [&](auto& g){ single_viewer.set_overlay_groups(g); },
                                nullptr, &status_msg);
                        else
                            load_overlay(overlay_file, left_overlays,
                                [&](auto& g){ compare.set_left_overlay_groups(g); },
                                nullptr, &status_msg);
                    }
                    if (use_single)
                        overlay_group_checkboxes(single_viewer, "ovg");
                    else
                        overlay_group_checkboxes(compare.left_viewer_ref(), "ovg", nullptr);
                }
            }

            ImGui::Separator();

            // ---- Tool tabs ----
            if (ImGui::BeginTabBar("##right_panel_tabs")) {

                // ---- Circle/Ellipse tab ----
                if (ImGui::BeginTabItem("Circle/Ellipse")) {
                    ImGui::Checkbox("Show Overlay##ce", &ce_tool.visible);
                    ImGui::Separator();
                    const bool has_img = use_single ? single_viewer.has_image()
                                                    : compare.get_left_image_data().width > 0;
                    ImGui::BeginDisabled(!has_img || ce_tool.is_analyzing());
                    if (ImGui::Button("Detect", {-1, 0})) {
                        const image_data& src = use_single
                            ? single_viewer.get_image_data()
                            : compare.get_left_image_data();
                        ce_tool.start_analyze(src);
                    }
                    ImGui::EndDisabled();
                    if (ImGui::Button("Clear", {-1, 0})) {
                        ce_tool.clear_results();
                        status_msg = "Detection cleared";
                    }
                    ImGui::Separator();
                    ce_tool.render_panel();
                    ImGui::EndTabItem();
                }

                // ---- Remote Overlay tab ----
                if (ImGui::BeginTabItem("Remote Overlay")) {
                    ImGui::Checkbox("Show Overlay##rot", &rot.visible);
                    ImGui::Separator();
                    rot.render_panel();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::EndChild();
        }

        // ----- Bottom profile panel -----
        if (show_profile_panel) {
            // Anchor to the left edge of the content region (not viewer_origin.x),
            // so the panel spans the full width including the capture control panel.
            const float content_left_x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
            ImGui::SetCursorScreenPos({content_left_x, viewer_origin.y + viewer_h});
            const float avail_w = ImGui::GetContentRegionAvail().x;
            ImGui::BeginChild("##profile_bottom", {avail_w, profile_panel_h}, ImGuiChildFlags_Borders);
            const float graph_h = ImGui::GetContentRegionAvail().y;
            // 4 plots: [X full] [X zoomed] [Y full] [Y zoomed]
            const float graph_w = (ImGui::GetContentRegionAvail().x
                                   - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            if (use_single) {
                const auto& hi    = single_viewer.get_hover_info();
                const image_data* img = &single_viewer.get_image_data();
                const int cx = hi.valid ? hi.img_x : -1;
                const int cy = hi.valid ? hi.img_y : -1;
                const auto& vs = single_viewer.get_view_state();
                const int vis_x0 = static_cast<int>(-vs.pan_x / vs.zoom);
                const int vis_x1 = static_cast<int>((-vs.pan_x + viewer_w) / vs.zoom);
                const int vis_y0 = static_cast<int>(-vs.pan_y / vs.zoom);
                const int vis_y1 = static_cast<int>((-vs.pan_y + viewer_h) / vs.zoom);
                draw_profile("X Profile##xprof", true,  cy, {{img, IM_COL32(80, 200, 255, 220), cx}}, graph_w, graph_h, vis_x0, vis_x1);
                ImGui::SameLine();
                draw_profile("Y Profile##yprof", false, cx, {{img, IM_COL32(80, 200, 255, 220), cy}}, graph_w, graph_h, vis_y0, vis_y1);
            } else {
                const auto& hi    = compare.get_hover_info();
                const image_data* li = &compare.get_left_image_data();
                const image_data* ri = &compare.get_right_image_data();
                const int cx = hi.valid ? hi.img_x : -1;
                const int cy = hi.valid ? hi.img_y : -1;
                const auto& vs = compare.get_view_state();
                const int vis_x0 = static_cast<int>(-vs.pan_x / vs.zoom);
                const int vis_x1 = static_cast<int>((-vs.pan_x + viewer_w * 0.5f) / vs.zoom);
                const int vis_y0 = static_cast<int>(-vs.pan_y / vs.zoom);
                const int vis_y1 = static_cast<int>((-vs.pan_y + viewer_h) / vs.zoom);
                draw_profile("X Profile##xprof", true,  cy,
                    {{li, IM_COL32(80, 200, 255, 220), cx},
                     {ri, IM_COL32(255, 160,  60, 220), cx}}, graph_w, graph_h, vis_x0, vis_x1);
                ImGui::SameLine();
                draw_profile("Y Profile##yprof", false, cx,
                    {{li, IM_COL32(80, 200, 255, 220), cy},
                     {ri, IM_COL32(255, 160,  60, 220), cy}}, graph_w, graph_h, vis_y0, vis_y1);
            }

            ImGui::EndChild();
        }

        // ----- Overlay scatter graph panel -----
        if (show_overlay_graph) {
            const float content_left_x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
            ImGui::SetCursorScreenPos({content_left_x, viewer_origin.y + viewer_h + profile_panel_h});
            const float avail_w = ImGui::GetContentRegionAvail().x;
            // Force opaque background so the panel doesn't look transparent when maximized.
            ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg); bg.w = 1.0f;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
            ImGui::BeginChild("##overlay_graph", {avail_w, overlay_graph_h}, ImGuiChildFlags_Borders);

            // Top-edge drag-to-resize handle (inside child = correct z-order, no viewer conflict)
            {
                const float cw = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPos({0.0f, 0.0f});
                ImGui::InvisibleButton("##ovg_resize", {cw, 4.0f});
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (ImGui::IsItemActive())
                    ovg_panel_h = std::clamp(ovg_panel_h - ImGui::GetIO().MouseDelta.y, 80.0f, max_ovg_h);

                // ▲ maximize / ▼ shrink buttons at top-right
                const float fp = ImGui::GetStyle().FramePadding.x;
                const float sp = ImGui::GetStyle().ItemSpacing.x;
                const float bw = ImGui::CalcTextSize("▲").x + fp * 2.0f;
                ImGui::SetCursorPos({cw - bw * 2.0f - sp, 0.0f});
                if (ImGui::SmallButton("▲##ovge")) ovg_panel_h = max_ovg_h;
                ImGui::SameLine();
                if (ImGui::SmallButton("▼##ovgs"))
                    ovg_panel_h = std::clamp(ovg_panel_h - 120.0f, 80.0f, max_ovg_h);
            }

            const std::vector<roi_group>* src  = use_single ? &overlays : &left_overlays;
            const std::vector<uint8_t>*   gvis = nullptr;
            {
                image_viewer& ref = use_single ? single_viewer : compare.left_viewer_ref();
                if (ref.overlay_group_count() == src->size())
                    gvis = &ref.overlay_group_visibility;
            }

            if (src->empty()) {
                ImGui::TextDisabled("No overlay loaded");
            } else if (ImGui::BeginTabBar("##ovgtabs")) {
                for (size_t gi = 0; gi < src->size(); ++gi) {
                    if (gvis && (*gvis)[gi] == 0) continue;
                    const auto& g = (*src)[gi];
                    if (g.entries.empty()) continue;

                    // Collect scatter data
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
                    if (ImGui::BeginTabItem(tab_id)) {
                        // y1/y2 fit: set to data range this frame when Auto Scale is pressed.
                        bool   y1_force = false, y2_force = false;
                        double y1_lo = 0.0, y1_hi = 0.0, y2_lo = 0.0, y2_hi = 0.0;

                        // ---- Settings ----
                        if (ImGui::CollapsingHeader("Settings##ovgs")) {
                            ImGui::TextUnformatted("Series:");
                            ImGui::SameLine();
                            ImGui::Checkbox("dx##ovs",         &ovg_show_dx);
                            ImGui::SameLine();
                            ImGui::Checkbox("dy##ovs",         &ovg_show_dy);
                            ImGui::SameLine();
                            ImGui::Checkbox("angle##ovs",      &ovg_show_angle);
                            ImGui::SameLine();
                            ImGui::Checkbox("Regression##ovs", &ovg_show_fit);

                            ImGui::Checkbox("Reference line##ovs", &ovg_show_ref);
                            if (ovg_show_ref) {
                                ImGui::SameLine();
                                ImGui::TextUnformatted("  y =");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(80);
                                ImGui::InputDouble("##ovg_ra", &ovg_ref_a, 0.0, 0.0, "%.4f");
                                ImGui::SameLine();
                                ImGui::TextUnformatted("x +");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(80);
                                ImGui::InputDouble("##ovg_rb", &ovg_ref_b, 0.0, 0.0, "%.4f");
                            }

                            // Auto Scale buttons: compute data range and force-apply once
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
                                y1_force = fit_range(dxs, dys, ovg_show_dx, ovg_show_dy, y1_lo, y1_hi);

                            ImGui::TextUnformatted("Y2 (angle):");
                            ImGui::SameLine();
                            if (ImGui::Button("Auto Scale##y2"))
                                y2_force = fit_range(angles, {}, ovg_show_angle, false, y2_lo, y2_hi);
                        }

                        const int stat_lines = (ovg_show_fit ? 2 : 0) + (ovg_show_ref ? 2 : 0);
                        const float text_h = stat_lines > 0
                            ? ImGui::GetTextLineHeightWithSpacing() * stat_lines + ImGui::GetStyle().ItemSpacing.y
                            : 0.0f;
                        const float plot_h = ImGui::GetContentRegionAvail().y - text_h;
                        const float plot_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

                        const ImPlotFlags pf = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;

                        // Linear regression: returns {a, b} for y = a*x + b.
                        auto linreg = [&](const std::vector<double>& xs,
                                          const std::vector<double>& ys) -> std::pair<double,double> {
                            const int m = static_cast<int>(xs.size());
                            if (m < 2) return {0.0, ys.empty() ? 0.0 : ys[0]};
                            double sx = 0, sy = 0, sxx = 0, sxy = 0;
                            for (int i = 0; i < m; ++i) { sx += xs[i]; sy += ys[i]; sxx += xs[i]*xs[i]; sxy += xs[i]*ys[i]; }
                            const double denom = m * sxx - sx * sx;
                            if (std::abs(denom) < 1e-12) return {0.0, sy / m};
                            const double a = (m * sxy - sx * sy) / denom;
                            const double b = (sy - a * sx) / m;
                            return {a, b};
                        };

                        // Reference line error statistics per series/axis.
                        struct RefStat { double max_abs, mean, stddev; bool valid; };
                        auto compute_ref_stat = [&](const std::vector<double>& xs,
                                                    const std::vector<double>& ys) -> RefStat {
                            if (!ovg_show_ref || n < 1) return {0, 0, 0, false};
                            double sum = 0, sum_sq = 0, max_abs = 0;
                            for (int i = 0; i < n; ++i) {
                                const double e = ys[i] - (ovg_ref_a * xs[i] + ovg_ref_b);
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

                        // Pre-compute all regressions for formula display below plots.
                        const auto [a_dx_col,  b_dx_col]  = linreg(xs_col, dxs);
                        const auto [a_dy_col,  b_dy_col]  = linreg(xs_col, dys);
                        const auto [a_ang_col, b_ang_col] = linreg(xs_col, angles);
                        const auto [a_dx_row,  b_dx_row]  = linreg(xs_row, dxs);
                        const auto [a_dy_row,  b_dy_row]  = linreg(xs_row, dys);
                        const auto [a_ang_row, b_ang_row] = linreg(xs_row, angles);

                        auto dual_scatter = [&](const char* title, const std::vector<double>& xs,
                                                const char* xlabel,
                                                double a_dx, double b_dx,
                                                double a_dy, double b_dy,
                                                double a_ang, double b_ang) {
                            char pid[128];
                            std::snprintf(pid, sizeof(pid), "%s##%zu", title, gi);

                            // Opaque plot background so the panel doesn't look transparent.
                            {
                                const ImVec4 wbg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                                ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4(wbg.x, wbg.y, wbg.z, 1.0f));
                            }
                            if (ImPlot::BeginPlot(pid, {plot_w, plot_h}, pf)) {
                                ImPlot::SetupAxes(xlabel, "dx / dy",
                                                  ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
                                ImPlot::SetupAxis(ImAxis_Y2, "angle", ImPlotAxisFlags_None);
                                if (y1_force)
                                    ImPlot::SetupAxisLimits(ImAxis_Y1, y1_lo, y1_hi, ImGuiCond_Always);
                                if (y2_force)
                                    ImPlot::SetupAxisLimits(ImAxis_Y2, y2_lo, y2_hi, ImGuiCond_Always);

                                // Scatter points
                                if (ovg_show_dx) ImPlot::PlotScatter("dx", xs.data(), dxs.data(), n);
                                if (ovg_show_dy) ImPlot::PlotScatter("dy", xs.data(), dys.data(), n);
                                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                                if (ovg_show_angle) ImPlot::PlotScatter("angle", xs.data(), angles.data(), n);

                                // Shared x range for fit and reference lines
                                double xmin = 0.0, xmax = 0.0;
                                if ((ovg_show_fit || ovg_show_ref) && n >= 2) {
                                    xmin = *std::min_element(xs.begin(), xs.end());
                                    xmax = *std::max_element(xs.begin(), xs.end());
                                }

                                // Linear regression lines
                                if (ovg_show_fit && n >= 2) {
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
                                    if (ovg_show_dx)    plot_fit(a_dx,  b_dx,  ImAxis_Y1, {0.4f, 0.8f, 1.0f, 0.8f}, "dx fit");
                                    if (ovg_show_dy)    plot_fit(a_dy,  b_dy,  ImAxis_Y1, {0.4f, 1.0f, 0.5f, 0.8f}, "dy fit");
                                    if (ovg_show_angle) plot_fit(a_ang, b_ang, ImAxis_Y2, {1.0f, 0.7f, 0.3f, 0.8f}, "angle fit");
                                }

                                // User reference line  y = ovg_ref_a * x + ovg_ref_b  (on Y1)
                                // ImPlotItemFlags_NoFit keeps this line out of auto-fit
                                // so the Y1 scale stays driven by the scatter data.
                                if (ovg_show_ref && n >= 2) {
                                    const double ref_xs[2] = {xmin, xmax};
                                    const double ref_ys[2] = {ovg_ref_a*xmin + ovg_ref_b,
                                                               ovg_ref_a*xmax + ovg_ref_b};
                                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                                    ImPlot::SetNextLineStyle({1.0f, 0.9f, 0.0f, 1.0f}, 2.0f);
                                    char rid[64];
                                    std::snprintf(rid, sizeof(rid), "y=%.3fx%+.3f##ref_%zu",
                                                  ovg_ref_a, ovg_ref_b, gi);
                                    ImPlot::PlotLine(rid, ref_xs, ref_ys, 2, ImPlotItemFlags_NoFit);
                                }

                                // Nearest-point tooltip
                                if (ImPlot::IsPlotHovered() && n > 0) {
                                    const ImVec2 mp = ImGui::GetMousePos();
                                    int nearest = -1, nearest_series = -1;
                                    float best = 15.0f;
                                    for (int i = 0; i < n; ++i) {
                                        auto check = [&](double y, ImAxis ya, int sid) {
                                            const ImVec2 pt = ImPlot::PlotToPixels(xs[i], y, ImAxis_X1, ya);
                                            const float d = std::sqrt((pt.x-mp.x)*(pt.x-mp.x)+(pt.y-mp.y)*(pt.y-mp.y));
                                            if (d < best) { best = d; nearest = i; nearest_series = sid; }
                                        };
                                        if (ovg_show_dx)    check(dxs[i],    ImAxis_Y1, 0);
                                        if (ovg_show_dy)    check(dys[i],    ImAxis_Y1, 1);
                                        if (ovg_show_angle) check(angles[i], ImAxis_Y2, 2);
                                    }
                                    if (nearest >= 0) {
                                        const auto& e = g.entries[nearest];
                                        ImGui::BeginTooltip();
                                        ImGui::Text("x: %d  y: %d", e.x, e.y);
                                        ImGui::Separator();
                                        auto row_text = [&](const char* label, double val, bool highlight) {
                                            if (highlight) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 60, 255));
                                            ImGui::Text("%-6s %.6f", label, val);
                                            if (highlight) ImGui::PopStyleColor();
                                        };
                                        if (ovg_show_dx)    row_text("dx:",    e.dx,    nearest_series == 0);
                                        if (ovg_show_dy)    row_text("dy:",    e.dy,    nearest_series == 1);
                                        if (ovg_show_angle) row_text("angle:", e.angle, nearest_series == 2);
                                        ImGui::EndTooltip();
                                    }
                                }

                                ImPlot::EndPlot();
                            }
                            ImPlot::PopStyleColor(); // ImPlotCol_PlotBg
                        };

                        if (plot_h > 1.0f && plot_w > 1.0f) {
                            dual_scatter("Column", xs_col, "col",
                                         a_dx_col, b_dx_col, a_dy_col, b_dy_col, a_ang_col, b_ang_col);
                            ImGui::SameLine();
                            dual_scatter("Row",    xs_row, "row",
                                         a_dx_row, b_dx_row, a_dy_row, b_dy_row, a_ang_row, b_ang_row);
                        } else {
                            ImGui::TextDisabled("(enlarge panel to view plots)");
                        }

                        // Regression formulas displayed below the plots
                        if (ovg_show_fit) {
                            ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Col:");
                            ImGui::SameLine(); ImGui::Text("dx=%.4fx%+.4f", a_dx_col,  b_dx_col);
                            ImGui::SameLine(); ImGui::Text("  dy=%.4fx%+.4f", a_dy_col,  b_dy_col);
                            ImGui::SameLine(); ImGui::TextColored({1.0f,0.7f,0.3f,1.0f},
                                                                  "  angle=%.4fx%+.4f", a_ang_col, b_ang_col);

                            ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Row:");
                            ImGui::SameLine(); ImGui::Text("dx=%.4fx%+.4f", a_dx_row,  b_dx_row);
                            ImGui::SameLine(); ImGui::Text("  dy=%.4fx%+.4f", a_dy_row,  b_dy_row);
                            ImGui::SameLine(); ImGui::TextColored({1.0f,0.7f,0.3f,1.0f},
                                                                  "  angle=%.4fx%+.4f", a_ang_row, b_ang_row);
                        }

                        // Reference line error statistics
                        if (ovg_show_ref) {
                            constexpr ImVec4 kRefCol = {1.0f, 0.9f, 0.0f, 1.0f};
                            auto show_stat = [](const char* label, const RefStat& s) {
                                ImGui::SameLine();
                                ImGui::Text("%s max=%.4f  mean=%+.4f  σ=%.4f",
                                            label, s.max_abs, s.mean, s.stddev);
                            };
                            ImGui::TextColored(kRefCol, "Ref Col:");
                            if (ovg_show_dx) show_stat("dx:", rs_col_dx);
                            if (ovg_show_dy) show_stat("dy:", rs_col_dy);

                            ImGui::TextColored(kRefCol, "Ref Row:");
                            if (ovg_show_dx) show_stat("dx:", rs_row_dx);
                            if (ovg_show_dy) show_stat("dy:", rs_row_dy);
                        }

                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(); // ImGuiCol_ChildBg
        }

        // Reset cursor to below all panels so the status bar sits correctly.
        ImGui::SetCursorScreenPos({viewer_origin.x,
            viewer_origin.y + viewer_h + profile_panel_h + overlay_graph_h});

        // ----- Status bar -----
        ImGui::Separator();
        ImGui::TextUnformatted(status_msg.empty()
            ? "Ready  |  Drop TIFF to open  |  Scroll: zoom  |  Ctrl+Scroll: pan H  |  Shift+Scroll: pan V  |  Drag: pan  |  Double-click: fit"
            : status_msg.c_str());

        ImGui::End();

        // ----- Loading progress overlay -----
        if (left_loader.active || right_loader.active) {
            ImGui::SetNextWindowPos(
                {static_cast<float>(win_w) * 0.5f, static_cast<float>(win_h) * 0.5f},
                ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::SetNextWindowSize({320.0f, 0.0f});
            ImGui::Begin("##loading_overlay", nullptr,
                ImGuiWindowFlags_NoDecoration    | ImGuiWindowFlags_NoMove        |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoNav);
            if (left_loader.active) {
                ImGui::TextUnformatted("Loading...");
                ImGui::ProgressBar(left_loader.progress.load(), {-1.0f, 0.0f});
            }
            if (right_loader.active) {
                if (left_loader.active) ImGui::Spacing();
                ImGui::TextUnformatted("Loading (right)...");
                ImGui::ProgressBar(right_loader.progress.load(), {-1.0f, 0.0f});
            }
            ImGui::End();
        }

        // ----- Log window -----
        if (show_log)
            app_log.draw("Log##log_win", &show_log);

        // Render
        ImGui::Render();
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Save window size and imgui layout to visionstudio.json.
    {
        int cur_w, cur_h;
        glfwGetWindowSize(window, &cur_w, &cur_h);
        const std::string imgui_ini = ImGui::SaveIniSettingsToMemory();

        nlohmann::json j = nlohmann::json::object();
        {
            std::ifstream jf("visionstudio.json");
            if (jf.is_open()) {
                try { j = nlohmann::json::parse(jf); } catch (...) {}
            }
        }
        j["window"]    = {{"width", cur_w}, {"height", cur_h}};
        j["imgui_ini"] = imgui_ini;
        j["overlay_graph"] = {
            {"show_dx",    ovg_show_dx},
            {"show_dy",    ovg_show_dy},
            {"show_angle", ovg_show_angle},
            {"show_fit",   ovg_show_fit},
            {"show_ref",   ovg_show_ref},
            {"ref_a",      ovg_ref_a},
            {"ref_b",      ovg_ref_b},
        };
        std::ofstream jf("visionstudio.json");
        if (jf.is_open()) jf << j.dump(2) << '\n';
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    NFD::Quit();
    return 0;
}
