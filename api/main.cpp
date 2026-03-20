#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "gui/compare_viewer.h"
#include "gui/image_viewer.h"
#include "io/tiff_io.h"
#include "util/image_data.h"

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

enum class app_mode { single, compare, split };

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
    app_mode        mode          = app_mode::single;
    image_viewer*   single_viewer = nullptr;
    compare_viewer* compare       = nullptr;
    image_data*     left_image    = nullptr;
    image_data*     right_image   = nullptr;
    std::string*    status_msg    = nullptr;
    async_loader*   left_loader   = nullptr;
    async_loader*   right_loader  = nullptr;
};

static void drop_callback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<app_state*>(glfwGetWindowUserPointer(window));

    switch (app->mode) {
    case app_mode::single:
        app->left_loader->start(paths[0]);
        break;
    case app_mode::compare:
        app->left_loader->start(paths[0]);
        if (count >= 2) app->right_loader->start(paths[1]);
        break;
    case app_mode::split:
        app->left_loader->start(paths[0]);
        break;
    }
    *app->status_msg = "Loading...";
}

int main(int argc, char** argv) {
    // -------------------------------------------------------------------------
    // Argument parsing (CLI11)
    // -------------------------------------------------------------------------
    CLI::App cli{"VisionStudio - TIFF image viewer"};
    cli.set_version_flag("--version", "0.1.0");

    std::string mode_str = "single";
    std::string arg_left, arg_right;
    bool        arg_diff    = false;
    float       arg_amplify = 1.0f;

    cli.add_option("--mode", mode_str, "Viewer mode: single | compare | split")
       ->transform(CLI::IsMember({"single", "compare", "split"}, CLI::ignore_case));
    cli.add_option("left",       arg_left,    "Left image (or single image)");
    cli.add_option("-r,--right", arg_right,   "Right image (compare mode)");
    cli.add_flag("--diff",       arg_diff,    "Enable diff mode on startup (compare mode)");
    cli.add_option("--amplify",  arg_amplify, "Diff amplification factor (default: 1.0)")
       ->check(CLI::Range(1.0f, 20.0f));

    CLI11_PARSE(cli, argc, argv);

    const app_mode mode = (mode_str == "compare") ? app_mode::compare
                        : (mode_str == "split")   ? app_mode::split
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

    async_loader left_loader;
    async_loader right_loader;

    app_state drop_state{mode,
                         &single_viewer, &compare,
                         &left_image, &right_image,
                         &status_msg,
                         &left_loader, &right_loader};
    glfwSetWindowUserPointer(window, &drop_state);

    // Apply diff flags from args (compare / split mode)
    if (mode == app_mode::compare || mode == app_mode::split) {
        compare.diff_mode    = arg_diff;
        compare.diff_amplify = arg_amplify;
    }

    // Load images specified on command line
    if (!arg_left.empty() && !arg_right.empty() && mode == app_mode::compare) {
        left_loader.start(arg_left);
        right_loader.start(arg_right);
        status_msg = "Loading...";
    } else if (!arg_left.empty()) {
        left_loader.start(arg_left);
        status_msg = "Loading...";
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
                    ImGui::MenuItem("Show Grid",    nullptr, &single_viewer.show_grid);
                    ImGui::MenuItem("Show Minimap", nullptr, &single_viewer.show_minimap);
                    if (single_viewer.show_grid) {
                        ImGui::Separator();
                        ImGui::SliderInt("Grid Spacing##s", &single_viewer.grid_spacing, 1, 500);
                    }
                } else {
                    ImGui::MenuItem("Show Grid",    nullptr, &compare.show_grid);
                    ImGui::MenuItem("Show Minimap", nullptr, &compare.show_minimap);
                    ImGui::MenuItem("Sync Views",   nullptr, &compare.sync_views);
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
