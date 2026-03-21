#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "gui/compare_viewer.h"
#include "gui/image_viewer.h"
#include "io/jsonl_io.h"
#include "io/tiff_io.h"
#include "util/image_data.h"
#include "util/roi_data.h"

#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>

static void glfw_error_cb(int error, const char* desc) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------

enum class app_mode { none, single, compare, split };

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
        break; // mode not selected yet, ignore drops
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

    cli.add_option("--mode", mode_str, "Viewer mode: single | compare | split")
       ->transform(CLI::IsMember({"single", "compare", "split"}, CLI::ignore_case));
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

    GLFWwindow* window = glfwCreateWindow(1280, 720, "VisionStudio", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, drop_callback);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
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
            ImGui::End();

            ImGui::Render();
            glViewport(0, 0, fb_w, fb_h);
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            continue;
        }

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
                if (mode == app_mode::single) {
                    ImGui::MenuItem("Show Grid",     nullptr, &single_viewer.show_grid);
                    ImGui::MenuItem("Show Minimap",  nullptr, &single_viewer.show_minimap);
                    ImGui::MenuItem("Show Overlays", nullptr, &single_viewer.show_overlays);
                    if (single_viewer.show_grid) {
                        ImGui::Separator();
                        ImGui::SliderInt("Grid Spacing##s", &single_viewer.grid_spacing, 1, 500);
                    }
                } else {
                    ImGui::MenuItem("Show Grid",     nullptr, &compare.show_grid);
                    ImGui::MenuItem("Show Minimap",  nullptr, &compare.show_minimap);
                    ImGui::MenuItem("Show Overlays", nullptr, &compare.show_overlays);
                    ImGui::MenuItem("Sync Views",    nullptr, &compare.sync_views);
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
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        if (open_file) ImGui::OpenPopup("##open_file");

        // ----- Open file popup (adapts to mode) -----
        if (ImGui::BeginPopupModal("##open_file", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            if (mode == app_mode::compare) {
                ImGui::Text("Left TIFF:");
                ImGui::SetNextItemWidth(400.0f);
                ImGui::InputText("##lp", left_path_buf, sizeof(left_path_buf));
                ImGui::Text("Right TIFF:");
                ImGui::SetNextItemWidth(400.0f);
                ImGui::InputText("##rp", right_path_buf, sizeof(right_path_buf));
            } else {
                ImGui::Text("TIFF path:");
                ImGui::SetNextItemWidth(400.0f);
                ImGui::InputText("##lp", left_path_buf, sizeof(left_path_buf));
            }

            if (ImGui::Button("Load")) {
                if (left_path_buf[0]) {
                    left_loader.start(left_path_buf);
                    if (mode == app_mode::compare && right_path_buf[0])
                        right_loader.start(right_path_buf);
                    status_msg = "Loading...";
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ----- Viewer area -----
        const float status_h = ImGui::GetFrameHeightWithSpacing();
        const float viewer_h = ImGui::GetContentRegionAvail().y - status_h;

        if (mode == app_mode::single) {
            single_viewer.render("single_canvas", 0.0f, viewer_h);
        } else {
            compare.render(0.0f, viewer_h);
        }

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

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
