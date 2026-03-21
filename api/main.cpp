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
#include "io/jsonl_io.h"
#include "io/tiff_io.h"
#include "util/image_data.h"
#include "util/roi_data.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <string>

static void glfw_error_cb(int error, const char* desc) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------

enum class app_mode { none, single, compare, split, capture };

// ---------------------------------------------------------------------------
// Async loader
// ---------------------------------------------------------------------------

struct async_loader {
    std::future<image_data> future;
    std::atomic<float>      progress{0.0f};
    bool                    active = false;
    std::string             path;

    async_loader()                               = default;
    async_loader(const async_loader&)            = delete;
    async_loader& operator=(const async_loader&) = delete;

    void start(std::string p) {
        path = p;
        progress.store(0.0f);
        active = true;
        auto* prog = &progress;
        future = std::async(std::launch::async, [p = std::move(p), prog]() {
            image_data img;
            tiff_io::read(p, img, prog);
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
    app_mode*                mode          = nullptr;
    image_viewer*            single_viewer = nullptr;
    compare_viewer*          compare       = nullptr;
    image_data*              left_image    = nullptr;
    image_data*              right_image   = nullptr;
    std::string*             status_msg    = nullptr;
    async_loader*            left_loader   = nullptr;
    async_loader*            right_loader  = nullptr;
    std::vector<roi_entry>*  overlays       = nullptr; // single mode
    std::vector<roi_entry>*  left_overlays  = nullptr; // compare / split mode (left panel)
    std::vector<roi_entry>*  right_overlays = nullptr; // compare mode (right panel)
};

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

    switch (*app->mode) {
    case app_mode::none:
    case app_mode::capture:
        break; // ignore drops
    case app_mode::single:
        for (int i = 0; i < count; ++i) {
            if (has_ext(paths[i], ".jsonl")) {
                if (jsonl_io::load(paths[i], *app->overlays)) {
                    app->single_viewer->set_overlays(*app->overlays);
                    *app->status_msg = std::string("Overlay loaded: ") + paths[i];
                }
            } else {
                app->left_loader->start(paths[i]);
                *app->status_msg = "Loading...";
            }
        }
        return; // status_msg already set above
    case app_mode::compare: {
        // Partition dropped files: .jsonl → overlays, others → images
        std::vector<const char*> jsonl_files, img_files;
        for (int i = 0; i < count; ++i)
            (has_ext(paths[i], ".jsonl") ? jsonl_files : img_files).push_back(paths[i]);
        if (!img_files.empty())  app->left_loader->start(img_files[0]);
        if (img_files.size() >= 2) app->right_loader->start(img_files[1]);
        if (!jsonl_files.empty()) {
            jsonl_io::load(jsonl_files[0], *app->left_overlays);
            app->compare->set_left_overlays(*app->left_overlays);
        }
        if (jsonl_files.size() >= 2) {
            jsonl_io::load(jsonl_files[1], *app->right_overlays);
            app->compare->set_right_overlays(*app->right_overlays);
        }
        *app->status_msg = "Loading...";
        return;
    }
    case app_mode::split: {
        for (int i = 0; i < count; ++i) {
            if (has_ext(paths[i], ".jsonl")) {
                jsonl_io::load(paths[i], *app->left_overlays);
                app->compare->set_split_overlays(*app->left_overlays);
                *app->status_msg = std::string("Overlay loaded: ") + paths[i];
            } else {
                app->left_loader->start(paths[i]);
                *app->status_msg = "Loading...";
            }
        }
        return;
    }
    }
    *app->status_msg = "Loading...";
}

int main(int argc, char** argv) {
    // -------------------------------------------------------------------------
    // Argument parsing (CLI11)
    // -------------------------------------------------------------------------
    CLI::App cli{"VisionStudio - TIFF image viewer"};
    cli.set_version_flag("--version", "0.1.0");

    std::string              mode_str;
    std::vector<std::string> arg_images;
    std::vector<std::string> arg_overlays;
    bool                     arg_diff    = false;
    float                    arg_amplify = 1.0f;

    cli.add_option("--mode", mode_str, "Viewer mode: single | compare | split | capture")
       ->transform(CLI::IsMember({"single", "compare", "split", "capture"}, CLI::ignore_case));
    cli.add_option("images", arg_images, "Image file(s). One file: single/split. Two files: compare.")
       ->expected(0, 2);
    cli.add_flag("--diff",      arg_diff,    "Enable diff mode on startup");
    cli.add_option("--amplify", arg_amplify, "Diff amplification factor (default: 1.0)")
       ->check(CLI::Range(1.0f, 20.0f));
    cli.add_option("--overlay", arg_overlays,
                   "JSONL overlay file(s). One file: single/split. Two files: compare left+right.")
       ->expected(1, 2)
       ->check(CLI::ExistingFile);

    CLI11_PARSE(cli, argc, argv);

    // --mode omitted → show mode selection UI at startup
    app_mode mode = mode_str.empty()          ? app_mode::none
                  : (mode_str == "compare")   ? app_mode::compare
                  : (mode_str == "split")     ? app_mode::split
                  : (mode_str == "capture")   ? app_mode::capture
                                              : app_mode::single;

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

    GLFWwindow* window = glfwCreateWindow(saved_w, saved_h, "VisionStudio", nullptr, nullptr);
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
    bool        show_camera_config    = false;
    bool        show_connect_config   = false;
    bool        server_connected      = false;

    // Capture settings
    int  capture_mode          = 0;     // 0=Capture, 1=Mode1, 2=Mode2
    bool image_acquisition     = true;
    bool live_image            = false;

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
    // Camera config editor state: one entry per file in cap_cfg.capture_config_files.
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
    std::vector<config_tab> config_tabs;         // capture_config_files
    int                     selected_config_tab  = 0;
    std::vector<config_tab> connect_tabs;        // connect_config_files
    int                     selected_connect_tab = 0;

    capture_config cap_cfg  = capture_config::load("visionstudio.json");
    capture_client cap_cli(cap_cfg);
    conn_edit      conn_buf = make_conn_edit(cap_cfg);

    // Build config editor tabs from capture_config_files.
    for (const auto& p : cap_cfg.capture_config_files) {
        config_tab t; t.path = p; t.load();
        config_tabs.push_back(std::move(t));
    }
    // Build connect editor tabs from connect_config_files.
    for (const auto& p : cap_cfg.connect_config_files) {
        config_tab t; t.path = p; t.load();
        connect_tabs.push_back(std::move(t));
    }

    async_loader           left_loader;
    async_loader           right_loader;
    std::vector<roi_entry> overlays;
    std::vector<roi_entry> left_overlays;
    std::vector<roi_entry> right_overlays;

    app_state drop_state{&mode,
                         &single_viewer, &compare,
                         &left_image, &right_image,
                         &status_msg,
                         &left_loader, &right_loader,
                         &overlays, &left_overlays, &right_overlays};
    glfwSetWindowUserPointer(window, &drop_state);

    // In capture mode launched via CLI, connect and start SSE automatically.
    if (mode == app_mode::capture) {
        if (cap_cli.connect_server())
            cap_cli.start_sse();
    }

    // Apply diff flags from args (compare / split mode)
    if (mode == app_mode::compare || mode == app_mode::split) {
        compare.diff_mode    = arg_diff;
        compare.diff_amplify = arg_amplify;
    }

    // Load images specified on command line
    if (!arg_images.empty()) {
        left_loader.start(arg_images[0]);
        if (arg_images.size() >= 2 && mode == app_mode::compare)
            right_loader.start(arg_images[1]);
        status_msg = "Loading...";
    }

    // Load overlay JSONL from CLI args
    if (!arg_overlays.empty()) {
        if (mode == app_mode::single) {
            if (jsonl_io::load(arg_overlays[0], overlays))
                single_viewer.set_overlays(overlays);
        } else if (mode == app_mode::split) {
            if (jsonl_io::load(arg_overlays[0], left_overlays))
                compare.set_split_overlays(left_overlays);
        } else if (mode == app_mode::compare) {
            if (jsonl_io::load(arg_overlays[0], left_overlays))
                compare.set_left_overlays(left_overlays);
            if (arg_overlays.size() >= 2 && jsonl_io::load(arg_overlays[1], right_overlays))
                compare.set_right_overlays(right_overlays);
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
        if (mode == app_mode::none) {
            ImGui::SetNextWindowPos(
                {static_cast<float>(win_w) * 0.5f, static_cast<float>(win_h) * 0.5f},
                ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowBgAlpha(0.92f);
            ImGui::Begin("##mode_select", nullptr,
                ImGuiWindowFlags_NoDecoration    | ImGuiWindowFlags_NoMove        |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoNav);
            ImGui::TextUnformatted("Select Viewer Mode");
            ImGui::Spacing();
            if (ImGui::Button("Single",  {120.0f, 40.0f})) mode = app_mode::single;
            ImGui::SameLine();
            if (ImGui::Button("Compare", {120.0f, 40.0f})) mode = app_mode::compare;
            ImGui::SameLine();
            if (ImGui::Button("Split",   {120.0f, 40.0f})) mode = app_mode::split;
            ImGui::SameLine();
            if (ImGui::Button("Capture", {120.0f, 40.0f}))
                mode = app_mode::capture;
            ImGui::End();

            ImGui::Render();
            glViewport(0, 0, fb_w, fb_h);
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            continue;
        }

        // In capture mode, the internal viewer is determined by capture_mode.
        const bool use_single = (mode == app_mode::single)
                             || (mode == app_mode::capture && capture_mode == 0);

        // ----- Poll async loaders -----
        {
            image_data tmp;
            if (left_loader.poll(tmp)) {
                left_image = std::move(tmp);
                switch (mode) {
                case app_mode::single:
                    single_viewer.load_image(left_image);
                    break;
                case app_mode::compare:
                    compare.load_left(left_image);
                    compare.left_label = left_loader.path;
                    break;
                case app_mode::split:
                    compare.load_split(left_image);
                    compare.left_label  = left_loader.path;
                    compare.right_label = left_loader.path;
                    break;
                case app_mode::capture:
                    if (capture_mode == 0) {
                        single_viewer.load_image(left_image);
                    } else if (capture_mode == 1) {
                        compare.load_split(left_image);
                        compare.left_label  = left_loader.path;
                        compare.right_label = left_loader.path;
                    } else {
                        compare.load_left(left_image);
                        compare.left_label = left_loader.path;
                    }
                    break;
                default:
                    break;
                }
                if (!right_loader.active)
                    status_msg = "Loaded: " + left_loader.path;
            }
            if (right_loader.poll(tmp)) {
                right_image = std::move(tmp);
                compare.load_right(right_image);
                compare.right_label = right_loader.path;
                status_msg          = "Loaded";
            }
        }

        // ----- Poll capture events (SSE) -----
        if (mode == app_mode::capture) {
            while (auto ev = cap_cli.poll_server_event()) {
                switch (ev->type) {
                case server_event_type::connected:
                    server_connected = true;
                    status_msg = "Server connected";
                    break;
                case server_event_type::disconnected:
                    server_connected = false;
                    cap_cli.stop_sse();
                    status_msg = "Server disconnected";
                    break;
                case server_event_type::error:
                    status_msg = "Server error: " + ev->message;
                    break;
                case server_event_type::capture_done:
                    if (capture_mode == 0) {
                        left_loader.start(ev->path);
                    } else if (capture_mode == 1) {
                        // Split: load same image into compare as split source
                        left_loader.start(ev->path);
                    } else {
                        // Compare: alternate left/right each capture
                        if (left_image.empty())
                            left_loader.start(ev->path);
                        else
                            right_loader.start(ev->path);
                    }
                    status_msg = "Capture complete: " + ev->path;
                    break;
                }
            }
            // Detect unexpected SSE drop (server crash etc.)
            if (server_connected && cap_cli.get_sse_state() == sse_state::error) {
                server_connected = false;
                cap_cli.stop_sse();
                status_msg = "Connection lost: " + cap_cli.get_last_error();
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
                if (mode != app_mode::capture)
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
                    ImGui::MenuItem("Show Tooltip",   nullptr, &single_viewer.show_coordinates);
                    ImGui::MenuItem("Show Crosshair", nullptr, &single_viewer.show_crosshair);
                    if (single_viewer.show_grid) {
                        ImGui::Separator();
                        ImGui::SliderInt("Grid Spacing##s", &single_viewer.grid_spacing, 1, 500);
                    }
                } else {
                    ImGui::MenuItem("Show Grid",     nullptr, &compare.show_grid);
                    ImGui::MenuItem("Show Minimap",  nullptr, &compare.show_minimap);
                    ImGui::MenuItem("Show Overlays",  nullptr, &compare.show_overlays);
                    ImGui::MenuItem("Show Tooltip",   nullptr, &compare.show_coordinates);
                    ImGui::MenuItem("Show Crosshair", nullptr, &compare.show_crosshair);
                    ImGui::MenuItem("Sync Views",     nullptr, &compare.sync_views);
                    if (mode == app_mode::split && compare.is_split()) {
                        ImGui::Separator();
                        ImGui::SetNextItemWidth(200.0f);
                        ImGui::SliderInt("Split##sp", &compare.split_x,
                                         1, compare.split_src_width() - 1);
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
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // ----- Open file via native dialog -----
        if (open_file) {
            constexpr nfdfilteritem_t kTiffFilter[] = {{"TIFF Image", "tiff,tif"}};
            if (mode == app_mode::compare) {
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
            if (!config_tabs.empty()) {
                // File selector
                if (config_tabs.size() > 1) {
                    std::vector<const char*> names;
                    for (const auto& t : config_tabs) names.push_back(t.path.c_str());
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##cfg_sel", &selected_config_tab, names.data(),
                                 static_cast<int>(names.size()));
                }

                auto& tab = config_tabs[selected_config_tab];

                // Path row
                ImGui::SetNextItemWidth(-180.0f);
                if (ImGui::InputText("##path", &tab.path)) tab.modified = true;
                ImGui::SameLine();
                if (ImGui::Button("Browse##cfg")) {
                    nfdchar_t* out = nullptr;
                    if (NFD::OpenDialog(out) == NFD_OKAY) {
                        tab.path = out;
                        NFD::FreePath(out);
                        tab.load();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Reload")) tab.load();

                // Text editor
                const float avail_h = ImGui::GetContentRegionAvail().y
                                    - ImGui::GetFrameHeightWithSpacing() - 4;
                if (ImGui::InputTextMultiline("##ed", &tab.text, {-1, avail_h}))
                    tab.modified = true;

                // Save button
                const std::string save_label = std::string("Save")
                    + (tab.modified ? " *" : "") + "##save";
                if (ImGui::Button(save_label.c_str())) tab.save();
            }
            ImGui::EndPopup();
        }

        // ----- Connect config editor modal -----
        if (show_connect_config) ImGui::OpenPopup("Connect Config##modal");
        ImGui::SetNextWindowSize({700, 540}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Connect Config##modal", &show_connect_config,
                                    ImGuiWindowFlags_NoResize)) {
            if (!connect_tabs.empty()) {
                if (connect_tabs.size() > 1) {
                    std::vector<const char*> names;
                    for (const auto& t : connect_tabs) names.push_back(t.path.c_str());
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##conn_sel", &selected_connect_tab, names.data(),
                                 static_cast<int>(names.size()));
                }

                auto& tab = connect_tabs[selected_connect_tab];

                ImGui::SetNextItemWidth(-180.0f);
                if (ImGui::InputText("##path", &tab.path)) tab.modified = true;
                ImGui::SameLine();
                if (ImGui::Button("Browse##conn_cfg")) {
                    nfdchar_t* out = nullptr;
                    if (NFD::OpenDialog(out) == NFD_OKAY) {
                        tab.path = out;
                        NFD::FreePath(out);
                        tab.load();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Reload##conn")) tab.load();

                const float avail_h = ImGui::GetContentRegionAvail().y
                                    - ImGui::GetFrameHeightWithSpacing() - 4;
                if (ImGui::InputTextMultiline("##ed", &tab.text, {-1, avail_h}))
                    tab.modified = true;

                const std::string save_label = std::string("Save")
                    + (tab.modified ? " *" : "") + "##connsave";
                if (ImGui::Button(save_label.c_str())) tab.save();
            }
            ImGui::EndPopup();
        }

        // ----- Viewer area -----
        const float status_h        = ImGui::GetFrameHeightWithSpacing();
        const float profile_panel_h = show_profile_panel ? 180.0f : 0.0f;
        const float viewer_h        = ImGui::GetContentRegionAvail().y - status_h - profile_panel_h;

        constexpr float panel_w         = 240.0f;  // right pixel panel
        constexpr float capture_panel_w = 180.0f;  // left capture control panel
        const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
        const float avail_x   = ImGui::GetContentRegionAvail().x;

        const float left_w  = (mode == app_mode::capture) ? capture_panel_w + spacing_x : 0.0f;
        const float right_w = show_pixel_panel ? panel_w + spacing_x : 0.0f;
        const float viewer_w = (left_w > 0.0f || right_w > 0.0f)
            ? avail_x - left_w - right_w
            : 0.0f;

        // ----- Left capture control panel -----
        if (mode == app_mode::capture) {
            if (ImGui::BeginChild("##capture_ctrl", {capture_panel_w, viewer_h},
                                  ImGuiChildFlags_Borders)) {
                // SSE status indicator
                const char* sse_label = "";
                ImVec4      sse_col   = {1, 1, 1, 1};
                switch (cap_cli.get_sse_state()) {
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
                    ImGui::BeginDisabled(server_connected);
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
                        cap_cli.update_config(cap_cfg);
                    }
                    if (!connect_tabs.empty()) {
                        ImGui::Separator();
                        ImGui::TextDisabled("Connect Config File");
                        for (int i = 0; i < static_cast<int>(connect_tabs.size()); ++i) {
                            const auto& p   = connect_tabs[i].path;
                            const auto  pos = p.find_last_of("/\\");
                            const std::string fname = (pos == std::string::npos) ? p : p.substr(pos + 1);
                            ImGui::PushID(i);
                            if (ImGui::Button(fname.empty() ? "(no file)" : fname.c_str(), {-1, 0})) {
                                selected_connect_tab = i;
                                show_connect_config = true;
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndDisabled();
                }
                ImGui::Separator();

                // Connection buttons (blue)
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.15f, 0.45f, 0.75f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.22f, 0.58f, 0.90f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.10f, 0.32f, 0.55f, 1.0f});
                ImGui::BeginDisabled(server_connected);
                if (ImGui::Button("Connect", {-1, 0})) {
                    if (!cap_cli.connect_server())
                        status_msg = "Connect failed: " + cap_cli.get_last_error();
                    else { status_msg = "Connecting..."; cap_cli.start_sse(); }
                }
                ImGui::EndDisabled();

                ImGui::BeginDisabled(!server_connected);
                if (ImGui::Button("Disconnect", {-1, 0})) {
                    if (!cap_cli.disconnect_server())
                        status_msg = "Disconnect failed: " + cap_cli.get_last_error();
                    else {
                        cap_cli.stop_sse();
                        server_connected = false;
                        status_msg = "Server disconnected";
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopStyleColor(3);
                ImGui::Separator();

                // Capture Settings (collapsible)
                // In debug builds, always accessible; in release, requires connection.
#ifndef NDEBUG
                constexpr bool cap_settings_disabled = false;
#else
                const bool cap_settings_disabled = !server_connected;
#endif
                ImGui::BeginDisabled(cap_settings_disabled);
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4{0.35f, 0.35f, 0.35f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{0.45f, 0.45f, 0.45f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4{0.28f, 0.28f, 0.28f, 1.0f});
                const bool cap_settings_open = ImGui::CollapsingHeader("Capture Settings");
                ImGui::PopStyleColor(3);
                if (cap_settings_open) {
                    constexpr const char* kCaptureModes[] = {"Capture (Single)", "Mode1 (Split)", "Mode2 (Compare)"};
                    ImGui::TextDisabled("Capture Mode");
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    ImGui::Combo("##cap_mode", &capture_mode, kCaptureModes, 3);

                    ImGui::TextDisabled("Image Acquisition");
                    if (ImGui::RadioButton("Enable##acq",  image_acquisition))  image_acquisition = true;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Disable##acq", !image_acquisition)) image_acquisition = false;

                    ImGui::TextDisabled("Live Image");
                    if (ImGui::RadioButton("Enable##live",  live_image))  live_image = true;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Disable##live", !live_image)) live_image = false;

                    // Capture config files: path + browse + edit modal
                    ImGui::Separator();
                    ImGui::TextDisabled("Capture Config Files");
                    const float fw = ImGui::GetContentRegionAvail().x;
                    for (int i = 0; i < static_cast<int>(cap_cfg.capture_config_files.size()); ++i) {
                        ImGui::PushID(i);
                        // Filename label
                        const auto& fpath = cap_cfg.capture_config_files[i];
                        const auto  pos   = fpath.find_last_of("/\\");
                        const std::string fname = (pos == std::string::npos) ? fpath : fpath.substr(pos + 1);
                        if (ImGui::Button(fname.empty() ? "(no file)" : fname.c_str(), {-1, 0})) {
                            selected_config_tab = i;
                            show_camera_config = true;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndDisabled();
                ImGui::Separator();

                // Capture buttons
                ImGui::BeginDisabled(!server_connected);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.18f, 0.55f, 0.18f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.25f, 0.70f, 0.25f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.12f, 0.40f, 0.12f, 1.0f});
                if (ImGui::Button("Start Capture", {-1, 0})) {
                    if (!cap_cli.start_capture())
                        status_msg = "Start failed: " + cap_cli.get_last_error();
                    else
                        status_msg = "Capture started";
                }
                ImGui::PopStyleColor(3);

                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.60f, 0.15f, 0.15f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.78f, 0.20f, 0.20f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.45f, 0.10f, 0.10f, 1.0f});
                if (ImGui::Button("Stop Capture", {-1, 0})) {
                    if (!cap_cli.stop_capture())
                        status_msg = "Stop failed: " + cap_cli.get_last_error();
                    else
                        status_msg = "Capture stopped";
                }
                ImGui::PopStyleColor(3);
                ImGui::EndDisabled();
            }
            ImGui::EndChild();
            ImGui::SameLine();
        }

        const ImVec2 viewer_origin = ImGui::GetCursorScreenPos();

        if (use_single) {
            single_viewer.render("single_canvas", viewer_w, viewer_h);
        } else {
            compare.render(viewer_w, viewer_h);
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
        //   plot_id : unique ImPlot ID string (use "##..." to hide title)
        //   is_x    : true = horizontal scan at row `fixed`, false = vertical at col `fixed`
        //   compact : true = suppress axis labels / Y tick labels (for small pixel panel)
        auto draw_profile = [&](const char* plot_id, bool is_x, int fixed,
                                std::initializer_list<series_entry> series,
                                float gw, float gh, bool compact) {
            // Determine total number of pixels along the profile axis.
            int total = 0;
            for (const auto& s : series) {
                if (s.img && !s.img->empty()) {
                    total = is_x ? s.img->width : s.img->height;
                    break;
                }
            }

            const ImPlotFlags plot_flags =
                ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText;
            // Always hide tick labels to maximise plot area; grid lines are kept.
            constexpr ImPlotAxisFlags kAxFlags =
                ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels;

            if (ImPlot::BeginPlot(plot_id, {gw, gh}, plot_flags)) {
                ImPlot::SetupAxes(nullptr, nullptr, kAxFlags, kAxFlags);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, total > 1 ? total - 1 : 1,
                                        ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 255, ImGuiCond_Always);

                int sidx = 0;
                for (const auto& s : series) {
                    if (!s.img || s.img->empty() || fixed < 0 || total <= 0) { ++sidx; continue; }
                    // Build float luminance array using this series' own size.
                    const int n = is_x ? s.img->width : s.img->height;
                    std::vector<float> ys(n);
                    for (int i = 0; i < n; ++i) {
                        const auto px = is_x ? s.img->pixel_at(i, fixed)
                                             : s.img->pixel_at(fixed, i);
                        ys[i] = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
                    }
                    // Each series needs a unique label so ImPlot treats them separately.
                    char lbl[16]; snprintf(lbl, sizeof(lbl), "##lum%d", sidx);
                    ImPlot::SetNextLineStyle(ImGui::ColorConvertU32ToFloat4(s.color), 1.5f);
                    ImPlot::PlotLine(lbl, ys.data(), n);

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
            const float graph_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            if (use_single) {
                const auto& hi    = single_viewer.get_hover_info();
                const image_data* img = &single_viewer.get_image_data();
                const int cx = hi.valid ? hi.img_x : -1;
                const int cy = hi.valid ? hi.img_y : -1;
                ImGui::BeginGroup();
                draw_profile("X Profile##xprof_btm", true,  cy, {{img, IM_COL32(80, 200, 255, 220), cx}}, graph_w, graph_h, false);
                ImGui::EndGroup();
                ImGui::SameLine();
                ImGui::BeginGroup();
                draw_profile("Y Profile##yprof_btm", false, cx, {{img, IM_COL32(80, 200, 255, 220), cy}}, graph_w, graph_h, false);
                ImGui::EndGroup();
            } else {
                const auto& hi    = compare.get_hover_info();
                const image_data* li = &compare.get_left_image_data();
                const image_data* ri = &compare.get_right_image_data();
                const int cx = hi.valid ? hi.img_x : -1;
                const int cy = hi.valid ? hi.img_y : -1;
                ImGui::BeginGroup();
                draw_profile("X Profile##xprof_btm", true,  cy,
                    {{li, IM_COL32(80, 200, 255, 220), cx},
                     {ri, IM_COL32(255, 160,  60, 220), cx}}, graph_w, graph_h, false);
                ImGui::EndGroup();
                ImGui::SameLine();
                ImGui::BeginGroup();
                draw_profile("Y Profile##yprof_btm", false, cx,
                    {{li, IM_COL32(80, 200, 255, 220), cy},
                     {ri, IM_COL32(255, 160,  60, 220), cy}}, graph_w, graph_h, false);
                ImGui::EndGroup();
            }

            ImGui::EndChild();
        }

        // Reset cursor to below all panels so the status bar sits correctly.
        ImGui::SetCursorScreenPos({viewer_origin.x, viewer_origin.y + viewer_h + profile_panel_h});

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
