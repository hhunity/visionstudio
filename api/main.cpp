#include <glad/glad.h>
#include <nfd.hpp>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#include "capture/capture_client.h"
#include "capture/capture_config.h"
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
#include <fstream>
#include <future>
#include <optional>
#include <string>

static void glfw_error_cb(int error, const char* desc) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------

enum class view_mode  { none, single, split, compare };
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
            if (!tiff_io::read(p, img, prog, tiff_io::ReadOptions{.output_format = PixelFormat::rgba}))
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
    case view_mode::split: {
        for (int i = 0; i < count; ++i) {
            if (has_ext(paths[i], ".json")) {
                load_overlay(paths[i], *app->left_overlays,
                    [&](auto& g){ app->compare->set_split_overlay_groups(g); },
                    app->overlay_file, app->status_msg);
            } else {
                app->left_loader->start(paths[i]);
                *app->status_msg = "Loading...";
            }
        }
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
    img_sub->add_option("--mode", view_mode_str, "View mode: single | split | compare")
        ->transform(CLI::IsMember({"single", "split", "compare"}, CLI::ignore_case));
    img_sub->add_option("images", arg_images, "Image file(s). One file: single/split. Two files: compare.")
        ->expected(0, 2);
    img_sub->add_flag("--diff",      arg_diff,    "Enable diff mode on startup");
    img_sub->add_option("--amplify", arg_amplify, "Diff amplification factor (default: 1.0)")
        ->check(CLI::Range(1.0f, 20.0f));
    img_sub->add_option("--overlay", arg_overlays,
                        "JSONL overlay file(s). One file: single/split. Two files: compare left+right.")
        ->expected(1, 2)
        ->check(CLI::ExistingFile);

    auto* cap_sub = cli.add_subcommand("capture", "Remote capture mode");
    cap_sub->add_option("--mode", view_mode_str, "View mode: single | split | compare")
        ->transform(CLI::IsMember({"single", "split", "compare"}, CLI::ignore_case));

    CLI11_PARSE(cli, argc, argv);

    const bool input_decided = img_sub->parsed() || cap_sub->parsed();
    input_mode imode = cap_sub->parsed() ? input_mode::remote_capture : input_mode::read_img;
    view_mode  vmode = view_mode_str.empty()        ? view_mode::none
                     : (view_mode_str == "compare") ? view_mode::compare
                     : (view_mode_str == "split")   ? view_mode::split
                                                    : view_mode::single;

    // -------------------------------------------------------------------------
    // GLFW + OpenGL + ImGui init
    // -------------------------------------------------------------------------
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) return 1;

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
    if (!window) { glfwTerminate(); return 1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, drop_callback);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        return 1;
    }

    NFD::Init();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

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
    bool        show_camera_config    = false;
    bool        show_connect_config   = false;
    bool        show_about            = false;
    bool        show_version          = false;
    bool        show_settings         = false;
    bool        settings_fresh        = false;
    nlohmann::json settings_edit;
    sse_state   cur_sse               = sse_state::disconnected;
    bool        capturing             = false;

    // Preview texture (MJPEG live preview)
    GLuint preview_tex   = 0;
    int    preview_tex_w = 0;
    int    preview_tex_h = 0;

    // Capture settings
    // capture_mode mirrors vmode for remote_capture; changeable at runtime.
    int  capture_mode      = vmode == view_mode::compare ? 2
                           : vmode == view_mode::split   ? 1
                                                         : 0;
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

    // Apply diff flags from args (compare / split mode)
    if (vmode == view_mode::compare || vmode == view_mode::split) {
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
        } else if (vmode == view_mode::split) {
            load_overlay(arg_overlays[0], left_overlays,
                [&](auto& g){ compare.set_split_overlay_groups(g); },
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
                if (ImGui::Button("Split##img",   {120.0f, 40.0f})) { vmode = view_mode::split;   imode = input_mode::read_img; }
                ImGui::SameLine();
                if (ImGui::Button("Compare##img", {120.0f, 40.0f})) { vmode = view_mode::compare; imode = input_mode::read_img; }
            }
            if (!input_decided || imode == input_mode::remote_capture) {
                if (!input_decided) ImGui::Spacing();
                ImGui::TextDisabled("Remote Capture");
                if (ImGui::Button("Single##cap",  {120.0f, 40.0f})) { vmode = view_mode::single;  imode = input_mode::remote_capture; cap_cli.emplace(cap_cfg); }
                ImGui::SameLine();
                if (ImGui::Button("Split##cap",   {120.0f, 40.0f})) { vmode = view_mode::split;   imode = input_mode::remote_capture; cap_cli.emplace(cap_cfg); }
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
                    case view_mode::split:
                        compare.load_split(left_image);
                        compare.left_label  = left_loader.path;
                        compare.right_label = left_loader.path;
                        break;
                    default:
                        break;
                    }
                    if (!right_loader.active)
                        status_msg = "Loaded: " + left_loader.path;
                }
            }
            if (right_loader.poll(tmp)) {
                if (tmp.empty()) {
                    status_msg = "Load failed: " + right_loader.path;
                } else {
                    right_image = std::move(tmp);
                    compare.load_right(right_image);
                    compare.right_label = right_loader.path;
                    status_msg          = "Loaded";
                }
            }
        }

        // ----- Poll capture events (SSE) -----
        if (imode == input_mode::remote_capture) {
            while (auto ev = cap_cli->poll_server_event()) {
                if (std::get_if<evt_connected>(&*ev)) {
                    cur_sse    = sse_state::connected;
                    status_msg = "Connected";
                } else if (std::get_if<evt_disconnected>(&*ev)) {
                    cur_sse    = sse_state::disconnected;
                    capturing  = false;
                    status_msg = "Server disconnected";
                } else if (auto* e = std::get_if<evt_error>(&*ev)) {
                    cur_sse    = sse_state::error;
                    status_msg = "Server error: " + e->message;
                } else if (auto* e = std::get_if<evt_capture_done>(&*ev)) {
                    capturing = false;
                    if (vmode == view_mode::compare) {
                        if (left_image.empty())
                            left_loader.start(e->path);
                        else
                            right_loader.start(e->path);
                    } else {
                        left_loader.start(e->path);
                    }
                    status_msg = "Capture complete: " + e->path;
                }
            }

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
                    ImGui::MenuItem("Show Overlays",  nullptr, &compare.show_overlays);
                    if (compare.show_overlays)
                        overlay_group_menu_items(compare.left_viewer_ref(), "covg",
                            compare.is_split() ? &compare.right_viewer_ref() : nullptr);
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
                    if (vmode == view_mode::split && compare.is_split()) {
                        ImGui::Separator();
                        ImGui::SetNextItemWidth(200.0f);
                        ImGui::SliderInt("Split##sp", &compare.split_x,
                                         1, compare.split_src_width() - 1);
                        compare.split_dragging = ImGui::IsItemActive();
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
                ImGui::MenuItem("Pixel Panel",   nullptr, &show_pixel_panel);
                ImGui::MenuItem("Profile Panel", nullptr, &show_profile_panel);
                ImGui::MenuItem("Overlay Graph", nullptr, &show_overlay_graph);
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
                nfdchar_t* out = nullptr;
                if (NFD::OpenDialog(out, kTiffFilter, 1) == NFD_OKAY) {
                    std::strncpy(left_path_buf, out, sizeof(left_path_buf) - 1);
                    NFD::FreePath(out);
                    out = nullptr;
                    if (NFD::OpenDialog(out, kTiffFilter, 1) == NFD_OKAY) {
                        std::strncpy(right_path_buf, out, sizeof(right_path_buf) - 1);
                        NFD::FreePath(out);
                    }
                    left_loader.start(left_path_buf);
                    if (right_path_buf[0]) right_loader.start(right_path_buf);
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
        ImGui::SetNextWindowSize({700, 540}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Camera Config##modal", &show_camera_config,
                                    ImGuiWindowFlags_NoResize)) {
            config_editor_modal_body(capture_cfg_tab, "cfg");
            ImGui::EndPopup();
        }

        // ----- Connect config editor modal -----
        if (show_connect_config) ImGui::OpenPopup("Connect Config##modal");
        ImGui::SetNextWindowSize({700, 540}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Connect Config##modal", &show_connect_config,
                                    ImGuiWindowFlags_NoResize)) {
            config_editor_modal_body(connect_cfg_tab, "conn");
            ImGui::EndPopup();
        }

        // ----- Version modal -----
        if (show_version) ImGui::OpenPopup("Version##modal");
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
        ImGui::SetNextWindowSize({620, 520}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("About VisionStudio##modal", &show_about,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::Text("VisionStudio  v%s", VS_VERSION_STRING);
            ImGui::Separator();
            const float text_h = ImGui::GetContentRegionAvail().y
                                - ImGui::GetFrameHeightWithSpacing() - 4;
            ImGui::InputTextMultiline("##about_text",
                const_cast<char*>(kThirdPartyLicenses),
                sizeof(kThirdPartyLicenses),
                {-1, text_h},
                ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button("Close")) { show_about = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        // ----- Settings modal -----
        // On fresh open: populate settings_edit from cap_cfg so all fields are shown.
        if (show_settings && settings_fresh) {
            settings_fresh = false;
            settings_edit = nlohmann::json::object();
            settings_edit["capture"] = {
                {"host",                 cap_cfg.host},
                {"port",                 cap_cfg.port},
                {"connect_path",         cap_cfg.connect_path},
                {"start_path",           cap_cfg.start_path},
                {"stop_path",            cap_cfg.stop_path},
                {"disconnect_path",      cap_cfg.disconnect_path},
                {"sse_path",             cap_cfg.sse_path},
                {"preview_path",         cap_cfg.preview_path},
                {"preview_raw_path",     cap_cfg.preview_raw_path},
                {"preview_raw",          cap_cfg.preview_raw},
                {"timeout_ms",           cap_cfg.timeout_ms},
                {"connect_config_file",  cap_cfg.connect_config_file},
                {"capture_config_file",  cap_cfg.capture_config_file},
            };
            // save_dir is managed separately (folder picker UI)
            settings_edit["save_dir"] = cap_cfg.save_dir;
        }
        if (show_settings) ImGui::OpenPopup("Settings##modal");
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
                    if (NFD::PickFolder(out) == NFD_OKAY) {
                        settings_edit["save_dir"] = std::string(out);
                        NFD::FreePath(out);
                    }
                }
                ImGui::Unindent(8.0f);
            }

            ImGui::Separator();
            const float btn_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Apply & Save", {btn_w, 0})) {
                // Update cap_cfg from settings_edit["capture"]
                if (settings_edit.contains("capture")) {
                    auto& c = settings_edit["capture"];
                    if (c.contains("host")               && c["host"].is_string())           cap_cfg.host               = c["host"].get<std::string>();
                    if (c.contains("port")               && c["port"].is_number_integer())   cap_cfg.port               = c["port"].get<int>();
                    if (c.contains("connect_path")       && c["connect_path"].is_string())   cap_cfg.connect_path       = c["connect_path"].get<std::string>();
                    if (c.contains("start_path")         && c["start_path"].is_string())     cap_cfg.start_path         = c["start_path"].get<std::string>();
                    if (c.contains("stop_path")          && c["stop_path"].is_string())      cap_cfg.stop_path          = c["stop_path"].get<std::string>();
                    if (c.contains("disconnect_path")    && c["disconnect_path"].is_string())cap_cfg.disconnect_path    = c["disconnect_path"].get<std::string>();
                    if (c.contains("sse_path")           && c["sse_path"].is_string())       cap_cfg.sse_path           = c["sse_path"].get<std::string>();
                    if (c.contains("preview_path")       && c["preview_path"].is_string())   cap_cfg.preview_path       = c["preview_path"].get<std::string>();
                    if (c.contains("preview_raw_path")   && c["preview_raw_path"].is_string())cap_cfg.preview_raw_path  = c["preview_raw_path"].get<std::string>();
                    if (c.contains("preview_raw")        && c["preview_raw"].is_boolean())   cap_cfg.preview_raw        = c["preview_raw"].get<bool>();
                    if (c.contains("timeout_ms")         && c["timeout_ms"].is_number_integer()) cap_cfg.timeout_ms     = c["timeout_ms"].get<int>();
                    if (c.contains("connect_config_file")&& c["connect_config_file"].is_string())cap_cfg.connect_config_file = c["connect_config_file"].get<std::string>();
                    if (c.contains("capture_config_file")&& c["capture_config_file"].is_string())cap_cfg.capture_config_file = c["capture_config_file"].get<std::string>();
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
            bool&  show_grid      = use_single ? single_viewer.show_grid      : compare.show_grid;
            bool&  show_minimap   = use_single ? single_viewer.show_minimap   : compare.show_minimap;
            bool&  show_overlays  = use_single ? single_viewer.show_overlays  : compare.show_overlays;
            bool&  show_tooltip   = use_single ? single_viewer.show_coordinates : compare.show_coordinates;
            bool&  show_crosshair = use_single ? single_viewer.show_crosshair : compare.show_crosshair;

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
            }
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            toggle_btn("Pixel",   show_pixel_panel);
            toggle_btn("Profile", show_profile_panel);
            toggle_btn("OvGraph", show_overlay_graph);
            ImGui::NewLine();
        }

        // ----- Viewer area -----
        const float status_h          = ImGui::GetFrameHeightWithSpacing();
        const float profile_panel_h   = show_profile_panel  ? 180.0f : 0.0f;
        const float overlay_graph_h   = show_overlay_graph  ? 360.0f : 0.0f;
        const float viewer_h          = ImGui::GetContentRegionAvail().y - status_h
                                        - profile_panel_h - overlay_graph_h;

        constexpr float panel_w         = 240.0f;  // right pixel panel
        constexpr float capture_panel_w = 180.0f;  // left capture control panel
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
                    const float fw = ImGui::GetContentRegionAvail().x;
                    bool conn_changed = false;
                    auto labeled = [&](const char* label, auto fn) {
                        ImGui::TextDisabled("%s", label);
                        ImGui::SetNextItemWidth(fw);
                        if (fn()) conn_changed = true;
                    };
                    labeled("Host",        [&]{ return ImGui::InputText("##host",       conn_buf.host,            sizeof(conn_buf.host)); });
                    labeled("Port",        [&]{ return ImGui::InputInt ("##port",       &conn_buf.port,           0); });
                    ImGui::Separator();
                    labeled("Connect",     [&]{ return ImGui::InputText("##conn_path",  conn_buf.connect_path,    sizeof(conn_buf.connect_path)); });
                    labeled("Start",       [&]{ return ImGui::InputText("##start_path", conn_buf.start_path,      sizeof(conn_buf.start_path)); });
                    labeled("Stop",        [&]{ return ImGui::InputText("##stop_path",  conn_buf.stop_path,       sizeof(conn_buf.stop_path)); });
                    labeled("Disconnect",  [&]{ return ImGui::InputText("##disc_path",  conn_buf.disconnect_path, sizeof(conn_buf.disconnect_path)); });
                    labeled("SSE",         [&]{ return ImGui::InputText("##sse_path",   conn_buf.sse_path,        sizeof(conn_buf.sse_path)); });
                    labeled("Timeout(ms)", [&]{ return ImGui::InputInt ("##timeout",    &conn_buf.timeout_ms,     0); });
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
                    constexpr const char* kCaptureModes[] = {"Single", "Split", "Compare"};
                    ImGui::TextDisabled("View Mode");
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    const int prev_cap_mode = capture_mode;
                    if (ImGui::Combo("##cap_mode", &capture_mode, kCaptureModes, 3)) {
                        if (prev_cap_mode == 2 && capture_mode != 2) {
                            ref_img_path.clear();
                            compare.unload_left();
                        }
                        vmode = capture_mode == 2 ? view_mode::compare
                              : capture_mode == 1 ? view_mode::split
                                                  : view_mode::single;
                    }
                    ImGui::Separator();

                    ImGui::TextDisabled("Image Acquisition");
                    if (ImGui::RadioButton("Enable##acq",  image_acquisition))  image_acquisition = true;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Disable##acq", !image_acquisition)) image_acquisition = false;

                    ImGui::TextDisabled("Live Image");
                    if (ImGui::RadioButton("Enable##live",  live_image))  live_image = true;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Disable##live", !live_image)) live_image = false;

                    ImGui::TextDisabled("Auto Detect");
                    if (ImGui::RadioButton("Enable##ad",  auto_detect))  auto_detect = true;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Disable##ad", !auto_detect)) auto_detect = false;

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
        } else {
            compare.render(viewer_w, viewer_h);
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

        // ----- Shared helpers for profile panels -----
        auto draw_rgba = [](const char* id, const std::array<uint8_t, 4>& rgba) {
            const ImVec4 cv{rgba[0]/255.f, rgba[1]/255.f, rgba[2]/255.f, rgba[3]/255.f};
            ImGui::ColorButton(id, cv,
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, {16, 16});
            ImGui::SameLine();
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

        // ----- Pixel panel -----
        if (show_pixel_panel) {
            ImGui::SetCursorScreenPos({viewer_origin.x + viewer_w + spacing_x, viewer_origin.y});
            ImGui::BeginChild("##pixel_panel", {panel_w, viewer_h}, ImGuiChildFlags_Borders);

            if (use_single) {
                const auto& hi = single_viewer.get_hover_info();
                if (!hi.valid) {
                    ImGui::TextDisabled("--");
                } else {
                    ImGui::Text("pos  : (%d, %d)", hi.img_x, hi.img_y);
                    ImGui::Text("zoom : %.2fx", static_cast<double>(hi.zoom));
                    ImGui::Separator();
                    draw_rgba("##swatch", hi.rgba);
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
                    draw_rgba("##lswatch", hi.left_rgba);
                    ImGui::Spacing();
                    ImGui::TextDisabled(compare.diff_mode ? "Diff" : "Right");
                    draw_rgba("##rswatch", hi.right_rgba);
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
                    // Left
                    ImGui::TextDisabled("L");
                    ImGui::SetNextItemWidth(path_w);
                    ImGui::InputText("##ov_path_l", &left_overlay_file, ImGuiInputTextFlags_ReadOnly);
                    ImGui::SameLine();
                    if (ImGui::Button("Load##ovl", {load_w, 0}))
                        load_overlay(left_overlay_file, left_overlays,
                            [&](auto& g){ compare.set_left_overlay_groups(g); },
                            nullptr, &status_msg, "Overlay L loaded: ");
                    overlay_group_checkboxes(compare.left_viewer_ref(), "ovgl");
                    // Right
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
                                [&](auto& g){ compare.set_split_overlay_groups(g); },
                                nullptr, &status_msg);
                    }
                    if (use_single)
                        overlay_group_checkboxes(single_viewer, "ovg");
                    else // split: left/right share the same groups
                        overlay_group_checkboxes(compare.left_viewer_ref(), "ovg",
                            &compare.right_viewer_ref());
                }
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
            ImGui::BeginChild("##overlay_graph", {avail_w, overlay_graph_h}, ImGuiChildFlags_Borders);

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
                        const float text_h  = ImGui::GetTextLineHeightWithSpacing() * 2.0f
                                            + ImGui::GetStyle().ItemSpacing.y;
                        const float plot_h  = ImGui::GetContentRegionAvail().y - text_h;
                        const float plot_w  = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

                        constexpr ImPlotFlags    pf = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;
                        constexpr ImPlotAxisFlags af = ImPlotAxisFlags_AutoFit;

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

                        // Pre-compute all regressions for formula display below plots.
                        const auto [a_dx_col,  b_dx_col]  = linreg(xs_col, dxs);
                        const auto [a_dy_col,  b_dy_col]  = linreg(xs_col, dys);
                        const auto [a_ang_col, b_ang_col] = linreg(xs_col, angles);
                        const auto [a_dx_row,  b_dx_row]  = linreg(xs_row, dxs);
                        const auto [a_dy_row,  b_dy_row]  = linreg(xs_row, dys);
                        const auto [a_ang_row, b_ang_row] = linreg(xs_row, angles);

                        // One plot showing dx, dy (Y1) and angle (Y2) against an index axis.
                        // Legend items are clickable to hide/show individual series.
                        auto dual_scatter = [&](const char* title, const std::vector<double>& xs,
                                                const char* xlabel,
                                                double a_dx, double b_dx,
                                                double a_dy, double b_dy,
                                                double a_ang, double b_ang) {
                            char pid[128];
                            std::snprintf(pid, sizeof(pid), "%s##%zu", title, gi);
                            if (ImPlot::BeginPlot(pid, {plot_w, plot_h}, pf)) {
                                ImPlot::SetupAxes(xlabel, "dx / dy", af, af);
                                ImPlot::SetupAxis(ImAxis_Y2, "angle", af);

                                // Scatter points
                                ImPlot::PlotScatter("dx",    xs.data(), dxs.data(),    n);
                                ImPlot::PlotScatter("dy",    xs.data(), dys.data(),    n);
                                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                                ImPlot::PlotScatter("angle", xs.data(), angles.data(), n);

                                // Linear regression lines
                                if (n >= 2) {
                                    const double xmin = *std::min_element(xs.begin(), xs.end());
                                    const double xmax = *std::max_element(xs.begin(), xs.end());
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
                                    plot_fit(a_dx,  b_dx,  ImAxis_Y1, {0.4f, 0.8f, 1.0f, 0.8f}, "dx fit");
                                    plot_fit(a_dy,  b_dy,  ImAxis_Y1, {0.4f, 1.0f, 0.5f, 0.8f}, "dy fit");
                                    plot_fit(a_ang, b_ang, ImAxis_Y2, {1.0f, 0.7f, 0.3f, 0.8f}, "angle fit");
                                }

                                // Nearest-point tooltip — tracks which series is closest
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
                                        check(dxs[i],    ImAxis_Y1, 0);
                                        check(dys[i],    ImAxis_Y1, 1);
                                        check(angles[i], ImAxis_Y2, 2);
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
                                        row_text("dx:",    e.dx,    nearest_series == 0);
                                        row_text("dy:",    e.dy,    nearest_series == 1);
                                        row_text("angle:", e.angle, nearest_series == 2);
                                        ImGui::EndTooltip();
                                    }
                                }

                                ImPlot::EndPlot();
                            }
                        };

                        dual_scatter("Column", xs_col, "col",
                                     a_dx_col, b_dx_col, a_dy_col, b_dy_col, a_ang_col, b_ang_col);
                        ImGui::SameLine();
                        dual_scatter("Row",    xs_row, "row",
                                     a_dx_row, b_dx_row, a_dy_row, b_dy_row, a_ang_row, b_ang_row);

                        // Regression formulas displayed below the plots
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

                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
        }

        // Reset cursor to below all panels so the status bar sits correctly.
        ImGui::SetCursorScreenPos({viewer_origin.x, viewer_origin.y + viewer_h + profile_panel_h + overlay_graph_h});

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

        // Render
        ImGui::Render();
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Save window size to visionstudio.json.
    {
        int cur_w, cur_h;
        glfwGetWindowSize(window, &cur_w, &cur_h);

        // Load existing JSON to preserve other keys.
        nlohmann::json j = nlohmann::json::object();
        {
            std::ifstream jf("visionstudio.json");
            if (jf.is_open()) {
                try { j = nlohmann::json::parse(jf); } catch (...) {}
            }
        }
        j["window"] = {{"width", cur_w}, {"height", cur_h}};
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
