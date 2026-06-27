#include <glad/glad.h>
#include <nfd.hpp>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#include "capture/capture_client.h"
#include "util/async_loader.h"
#include "util/capture_config.h"
#include "util/config_tab.h"
#include "gui/app_context.h"
#include "gui/app_types.h"
#include "gui/capture_panel.h"
#include "gui/panel_base.h"
#include "gui/circle_ellipse_tool.h"
#include "gui/log_panel.h"
#include "gui/measure_tool.h"
#include "gui/overlay_graph_panel.h"
#include "gui/profile_panel.h"
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
    // Convert UTF-8 message to UTF-16 for MessageBoxW so Japanese characters render correctly.
    auto to_wide = [](const char* s) -> std::wstring {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
        return w;
    };
    MessageBoxW(nullptr, to_wide(msg).c_str(), L"VisionStudio - Fatal Error", MB_OK | MB_ICONERROR);
}
#else
static void fatal_error(const char* msg) { fprintf(stderr, "Fatal: %s\n", msg); }
#endif

static void glfw_error_cb(int error, const char* desc) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

// ---------------------------------------------------------------------------
// App state shared with GLFW drop callback
// ---------------------------------------------------------------------------

struct app_state {
    view_mode*               vmode         = nullptr;
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
    }
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

    const input_mode imode = cap_sub->parsed() ? input_mode::remote_capture : input_mode::read_img;
    view_mode  vmode = (view_mode_str == "compare") ? view_mode::compare
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
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

#ifdef VS_CJK_FONT
    {
        // Merge two Noto Sans fonts to support English, Japanese, and Korean.
        // Enabled at build time with -DVS_CJK_FONT=ON (adds ~1s startup rasterization).
        static const ImWchar jp_ranges[] = {
            0x0020, 0x00FF,  // Basic Latin + Latin Supplement (English)
            0x3000, 0x30FF,  // CJK Symbols, Hiragana, Katakana
            0x31F0, 0x31FF,  // Katakana Phonetic Extensions
            0x4E00, 0x9FFF,  // CJK Unified Ideographs (Kanji)
            0xFF00, 0xFFEF,  // Halfwidth / Fullwidth
            0,
        };
        static const ImWchar kr_ranges[] = {
            0x3131, 0x3163,  // Hangul Compatibility Jamo
            0xA960, 0xA97F,  // Hangul Jamo Extended-A
            0xAC00, 0xD7A3,  // Hangul Syllables
            0xD7B0, 0xD7FF,  // Hangul Jamo Extended-B
            0,
        };
        ImFontConfig merge_cfg;
        merge_cfg.MergeMode  = true;
        merge_cfg.OversampleH = 1;
        merge_cfg.OversampleV = 1;
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        atlas->AddFontFromFileTTF("assets/fonts/NotoSansJP-Regular.ttf", 14.0f, nullptr,   jp_ranges);
        atlas->AddFontFromFileTTF("assets/fonts/NotoSansKR-Regular.ttf", 14.0f, &merge_cfg, kr_ranges);
    }
#endif

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
    capture_panel       cap_panel;
    profile_panel       prof_panel;
    overlay_graph_panel ovg_panel;
    log_panel           app_log;
    float  viewer_pan_speed          = 32.0f;
    float  viewer_minimap_aspect     = 0.0f;
    float  toolbar_bottom_y_cached   = 0.0f;  // previous frame's toolbar bottom Y
    circle_ellipse_tool ce_tool;
    measure_tool        mt;
    remote_overlay_tool rot;
    bool        show_camera_config    = false;
    bool        show_connect_config   = false;
    bool        show_about            = false;
    bool        show_version          = false;
    std::string error_msg;
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

    config_tab capture_cfg_tab;   // capture_config_file
    config_tab connect_cfg_tab;   // connect_config_file

    // Load settings from visionstudio.json
    {
        std::ifstream jf("visionstudio.json");
        if (jf.is_open()) {
            try {
                const auto j = nlohmann::json::parse(jf);
                ovg_panel.load_settings(j);
                if (j.contains("viewer") && j["viewer"].is_object()) {
                    const auto& vw = j["viewer"];
                    if (vw.contains("pan_speed")      && vw["pan_speed"].is_number())
                        viewer_pan_speed      = static_cast<float>(vw["pan_speed"].get<double>());
                    if (vw.contains("minimap_aspect") && vw["minimap_aspect"].is_number())
                        viewer_minimap_aspect = static_cast<float>(vw["minimap_aspect"].get<double>());
                }
            } catch (...) {}
        }
    }
    single_viewer.pan_speed            = viewer_pan_speed;
    single_viewer.minimap_force_aspect = viewer_minimap_aspect;
    compare.pan_speed                  = viewer_pan_speed;
    compare.minimap_force_aspect       = viewer_minimap_aspect;

    capture_config                cap_cfg  = capture_config::load("visionstudio.json");
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

    app_state drop_state{&vmode,
                         &single_viewer, &compare,
                         &left_image, &right_image,
                         &status_msg,
                         &left_loader, &right_loader,
                         &overlays, &left_overlays, &right_overlays,
                         &overlay_file, &left_overlay_file, &right_overlay_file};
    glfwSetWindowUserPointer(window, &drop_state);

    // Build the single shared app context (stable pointers set once).
    app_context ctx;
    ctx.single_viewer       = &single_viewer;
    ctx.compare             = &compare;
    ctx.overlays            = &overlays;
    ctx.left_overlays       = &left_overlays;
    ctx.left_loader         = &left_loader;
    ctx.right_loader        = &right_loader;
    ctx.cur_sse             = &cur_sse;
    ctx.capturing           = &capturing;
    ctx.cap_cfg             = &cap_cfg;
    ctx.conn_buf            = &conn_buf;
    ctx.capture_mode        = &capture_mode;
    ctx.vmode               = &vmode;
    ctx.image_acquisition   = &image_acquisition;
    ctx.live_image          = &live_image;
    ctx.auto_detect         = &auto_detect;
    ctx.ref_img_path        = &ref_img_path;
    ctx.show_camera_config  = &show_camera_config;
    ctx.show_connect_config = &show_connect_config;
    ctx.capture_cfg_tab     = &capture_cfg_tab;
    ctx.connect_cfg_tab     = &connect_cfg_tab;
    ctx.cam_info            = &cam_info;
    ctx.cam_info_future     = &cam_info_future;
    ctx.preview_tex         = &preview_tex;
    ctx.preview_tex_w       = &preview_tex_w;
    ctx.preview_tex_h       = &preview_tex_h;
    ctx.log                 = &app_log;

    // Init panels
    std::vector<panel_base*> panels = {&cap_panel, &prof_panel, &ovg_panel, &app_log};
    for (auto* p : panels) p->init(&ctx);

    // Capture panel is opt-in; show it only when --mode capture was passed.
    cap_panel.visible = (imode == input_mode::remote_capture);

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

        // Must run before any ImGui::Begin() so drag clamping takes effect
        // before ImGui processes window movement in the root window.
        for (auto* p : panels) p->pre_frame();

        int fb_w, fb_h, win_w, win_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glfwGetWindowSize(window, &win_w, &win_h);

        const bool use_single = (vmode == view_mode::single);

        // ----- Poll async loaders -----
        {
            image_data tmp;
            if (left_loader.poll(tmp)) {
                if (tmp.empty()) {
                    error_msg = "Failed to open:\n" + left_loader.path;
                    app_log.add("ERROR", ("Load failed: " + left_loader.path).c_str());
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
                        app_log.add("INFO", ("Loaded: " + left_loader.path).c_str());
                }
            }
            if (right_loader.poll(tmp)) {
                if (tmp.empty()) {
                    error_msg = "Failed to open:\n" + right_loader.path;
                    app_log.add("ERROR", ("Load failed: " + right_loader.path).c_str());
                } else {
                    right_image = std::move(tmp);
                    compare.load_right(right_image);
                    compare.right_label = right_loader.path;
                    app_log.add("INFO", ("Loaded (right): " + right_loader.path).c_str());
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
                app_log.add("INFO", "Remote overlay loaded");
            }
        }

        // ----- Upload preview frame to GPU (remote capture mode) -----
        // SSE event polling is handled inside cap_panel.render() via poll_events().
        if (cap_panel.visible) {
            preview_frame pf;
            if (cap_panel.poll_preview_frame(pf)) {
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

        // DockSpace starting below the toolbar so snap previews don't overlap it.
        {
            const float dock_y = toolbar_bottom_y_cached > 0.0f ? toolbar_bottom_y_cached : 0.0f;
            ImGui::SetNextWindowPos({0.0f, dock_y});
            ImGui::SetNextWindowSize({static_cast<float>(win_w), static_cast<float>(win_h) - dock_y});
            ImGui::SetNextWindowBgAlpha(0.0f);
            constexpr ImGuiWindowFlags kDockFlags =
                ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoCollapse     |
                ImGuiWindowFlags_NoResize           | ImGuiWindowFlags_NoMove         |
                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus  |
                ImGuiWindowFlags_NoBackground       | ImGuiWindowFlags_NoDocking;
            ImGui::Begin("##dockspace_host", nullptr, kDockFlags);
            ImGui::DockSpace(ImGui::GetID("##dockspace"), {0.0f, 0.0f},
                             ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::End();
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
                        if (ImGui::SliderFloat("Minimap Aspect##ms", &single_viewer.minimap_force_aspect, 0.0f, 10.0f, "%.1f"))
                            viewer_minimap_aspect = single_viewer.minimap_force_aspect;
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
                        if (ImGui::SliderFloat("Minimap Aspect##mc", &compare.minimap_force_aspect, 0.0f, 10.0f, "%.1f"))
                            viewer_minimap_aspect = compare.minimap_force_aspect;
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
                ImGui::MenuItem("Capture Panel", nullptr, &cap_panel.visible);
                ImGui::MenuItem("Pixel Panel",      nullptr, &show_pixel_panel);
                ImGui::MenuItem("Profile Panel",    nullptr, &prof_panel.visible);
                ImGui::MenuItem("Overlay Graph",    nullptr, &ovg_panel.visible);
                ImGui::MenuItem("Circle/Ellipse Overlay", nullptr, &ce_tool.visible);
                ImGui::MenuItem("Measure Tool",           nullptr, &mt.visible);
                ImGui::MenuItem("Remote Overlay",         nullptr, &rot.visible);
                ImGui::MenuItem("Log",              nullptr, &app_log.visible);
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
                    app_log.add("INFO", "Loading...");
                }
            } else {
                nfdchar_t* out = nullptr;
                if (NFD::OpenDialog(out, kTiffFilter, 1) == NFD_OKAY) {
                    std::strncpy(left_path_buf, out, sizeof(left_path_buf) - 1);
                    NFD::FreePath(out);
                    left_loader.start(left_path_buf);
                    app_log.add("INFO", "Loading...");
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
        if (cap_panel.visible && cur_sse == sse_state::connecting)
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
                cap_panel.cancel_connect();
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
            constexpr ImVec4 kOn  = {0.15f, 0.45f, 0.75f, 1.0f};
            constexpr ImVec4 kOnH = {0.25f, 0.55f, 0.85f, 1.0f};
            constexpr ImVec4 kOnA = {0.10f, 0.35f, 0.65f, 1.0f};

            auto toggle_btn = [&](const char* label, bool& flag) {
                const bool on = flag;
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kOn);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOnH);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kOnA);
                }
                if (ImGui::SmallButton(label)) flag = !flag;
                if (on) ImGui::PopStyleColor(3);
                ImGui::SameLine();
            };

            // ----- Single / Compare mode switch -----
            {
                const bool is_single = use_single;
                if (is_single) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kOn);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOnH);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kOnA);
                }
                if (ImGui::SmallButton("Single") && !is_single) {
                    vmode = view_mode::single;
                    if (!left_image.empty())  single_viewer.load_image(left_image);
                }
                if (is_single) ImGui::PopStyleColor(3);
                ImGui::SameLine();

                const bool is_compare = !use_single;
                if (is_compare) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kOn);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOnH);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kOnA);
                }
                if (ImGui::SmallButton("Compare") && !is_compare) {
                    vmode = view_mode::compare;
                    if (!left_image.empty())  compare.load_left(left_image);
                    if (!right_image.empty()) compare.load_right(right_image);
                }
                if (is_compare) ImGui::PopStyleColor(3);
                ImGui::SameLine();
            }
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            bool&  show_grid      = use_single ? single_viewer.show_grid        : compare.show_grid;
            bool&  show_minimap   = use_single ? single_viewer.show_minimap     : compare.show_minimap;
            bool&  show_overlays  = use_single ? single_viewer.show_overlays    : compare.show_left_overlays;
            bool&  show_tooltip   = use_single ? single_viewer.show_coordinates : compare.show_coordinates;
            bool&  show_crosshair = use_single ? single_viewer.show_crosshair   : compare.show_crosshair;

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
            toggle_btn("Capture", cap_panel.visible);
            toggle_btn("Pixel",   show_pixel_panel);
            toggle_btn("Profile", prof_panel.visible);
            toggle_btn("OvGraph", ovg_panel.visible);
            toggle_btn("Detect",  ce_tool.visible);
            toggle_btn("Measure", mt.visible);
            toggle_btn("Remote",  rot.visible);
            ImGui::NewLine();
        }

        // ----- Viewer area -----
        const float toolbar_bottom_y  = ImGui::GetCursorScreenPos().y;
        toolbar_bottom_y_cached = toolbar_bottom_y;  // used next frame for dockspace origin

        // Update per-frame context fields.
        ctx.min_y = toolbar_bottom_y;

        // ----- Left capture control panel (floating) -----
        cap_panel.render();

        ImGui::End(); // ##root

        // ----- Viewer floating window -----
        // Declare outside if-block so ctx can be updated after End().
        float viewer_w = 0.0f, viewer_h = 0.0f;
        ImGui::SetNextWindowPos({0.0f, toolbar_bottom_y_cached}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
            {static_cast<float>(win_w) * 0.75f,
             static_cast<float>(win_h) - toolbar_bottom_y_cached},
            ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Viewer##viewer_win", nullptr)) {
            static float panel_w  = 240.0f;
            const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
            const float avail_x   = ImGui::GetContentRegionAvail().x;
            const float avail_h   = ImGui::GetContentRegionAvail().y;
            const float right_w   = show_pixel_panel ? panel_w + spacing_x : 0.0f;
            viewer_w = right_w > 0.0f ? avail_x - right_w : avail_x;
            viewer_h = avail_h;

            const ImVec2 viewer_origin = ImGui::GetCursorScreenPos();

        if (cap_panel.visible && use_single && preview_tex != 0) {
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
            {
                const view_state& vs = single_viewer.get_view_state();
                auto* dl = ImGui::GetWindowDrawList();
                if (ce_tool.visible)
                    ce_tool.render_overlay(dl, viewer_origin, {viewer_w, viewer_h},
                                           vs.zoom, vs.pan_x, vs.pan_y);
                if (mt.visible) {
                    const auto& hi = single_viewer.get_hover_info();
                    mt.render_overlay(dl, viewer_origin, {viewer_w, viewer_h},
                                      vs.zoom, vs.pan_x, vs.pan_y,
                                      hi.valid ? hi.img_x : -1,
                                      hi.valid ? hi.img_y : -1);
                }
            }
        } else {
            compare.render(viewer_w, viewer_h);
            {
                const view_state& lvs    = compare.get_view_state();
                const float spacing      = ImGui::GetStyle().ItemSpacing.x;
                const float half_w       = std::floor((viewer_w - spacing) * 0.5f);
                const float header_h     = compare.get_header_height();
                const ImVec2 canvas_pos  = {viewer_origin.x, viewer_origin.y + header_h};
                const float  canvas_h    = viewer_h - header_h;
                const ImVec2 left_vmax   = {viewer_origin.x + half_w, viewer_origin.y + viewer_h};
                const bool left_hovered  = ImGui::IsMouseHoveringRect(viewer_origin, left_vmax);
                auto* dl = ImGui::GetWindowDrawList();
                if (ce_tool.visible)
                    ce_tool.render_overlay(dl, canvas_pos, {half_w, canvas_h},
                                           lvs.zoom, lvs.pan_x, lvs.pan_y);
                if (mt.visible) {
                    const auto& hi = compare.get_hover_info();
                    mt.render_overlay(dl, canvas_pos, {half_w, canvas_h},
                                      lvs.zoom, lvs.pan_x, lvs.pan_y,
                                      (hi.valid && left_hovered) ? hi.img_x : -1,
                                      (hi.valid && left_hovered) ? hi.img_y : -1);
                }
            }
        }

        // Measure tool click detection (after viewer renders so hover_info is current)
        if (mt.visible) {
            bool  hover_valid = false;
            int   hover_x = -1, hover_y = -1;
            if (use_single) {
                const auto& hi = single_viewer.get_hover_info();
                hover_valid = hi.valid;
                hover_x = hi.img_x; hover_y = hi.img_y;
            } else {
                // In compare mode, only accept clicks on the left panel.
                const float spacing    = ImGui::GetStyle().ItemSpacing.x;
                const float half_w     = std::floor((viewer_w - spacing) * 0.5f);
                const ImVec2 left_vmax = {viewer_origin.x + half_w, viewer_origin.y + viewer_h};
                const bool left_hovered = ImGui::IsMouseHoveringRect(viewer_origin, left_vmax);
                const auto& hi = compare.get_hover_info();
                hover_valid = hi.valid && left_hovered;
                hover_x = hi.img_x; hover_y = hi.img_y;
            }
            static ImVec2 s_mt_press_pos = {-1.0f, -1.0f};
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                s_mt_press_pos = ImGui::GetMousePos();
            if (hover_valid && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const ImVec2 cur = ImGui::GetMousePos();
                const float  threshold = ImGui::GetIO().MouseDragThreshold;
                if (std::abs(cur.x - s_mt_press_pos.x) < threshold &&
                    std::abs(cur.y - s_mt_press_pos.y) < threshold)
                    mt.handle_click(hover_x, hover_y);
            }
        }

        // Right-click context menu on viewer area
        {
            const ImVec2 vmin = viewer_origin;
            const ImVec2 vmax = {viewer_origin.x + viewer_w, viewer_origin.y + viewer_h};
            if (ImGui::IsMouseHoveringRect(vmin, vmax) &&
                ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##viewer_ctx");
            if (ImGui::BeginPopup("##viewer_ctx")) {
                ImGui::MenuItem("Measure Tool", nullptr, &mt.visible);
                if (mt.visible && ImGui::MenuItem("Reset Measure"))
                    mt.reset();
                ImGui::Separator();
                ImGui::MenuItem("Show Grid",     nullptr,
                    use_single ? &single_viewer.show_grid     : &compare.show_grid);
                ImGui::MenuItem("Show Minimap",  nullptr,
                    use_single ? &single_viewer.show_minimap  : &compare.show_minimap);
                ImGui::MenuItem("Show Overlays", nullptr,
                    use_single ? &single_viewer.show_overlays : &compare.show_left_overlays);
                ImGui::EndPopup();
            }
        }

        // For split/compare capture: overlay live preview on the right panel
        if (cap_panel.visible && !use_single && preview_tex != 0) {
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
            if (has_guide && cap_panel.visible && !cap_panel.is_preview_active()) {
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

        // ----- Resize handles -----
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
            {
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

                // ---- Measure tab ----
                if (ImGui::BeginTabItem("Measure")) {
                    ImGui::Checkbox("Show Overlay##mt", &mt.visible);
                    ImGui::Separator();
                    mt.render_panel();
                    ImGui::EndTabItem();
                }

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
                        app_log.add("INFO", "Detection cleared");
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

        }
        ImGui::End(); // Viewer##viewer_win

        // Update per-frame viewer context fields (outside the if-block so
        // floating panels always receive current values even when viewer is collapsed).
        ctx.use_single = use_single;
        ctx.viewer_w   = viewer_w;
        ctx.viewer_h   = viewer_h;

        // ----- Floating panels (always rendered, independent of viewer window) -----
        prof_panel.render();
        ovg_panel.render();

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
        app_log.render();

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
        j["viewer"]    = {{"pan_speed", viewer_pan_speed},
                          {"minimap_aspect", viewer_minimap_aspect}};
        j["overlay_graph"] = ovg_panel.save_settings();
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
