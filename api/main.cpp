#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "gui/compare_viewer.h"
#include "gui/image_viewer.h"
#include "io/tiff_io.h"
#include "util/image_data.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>

static void glfw_error_cb(int error, const char* desc) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

// ---------------------------------------------------------------------------
// Async loader: reads a TIFF on a background thread, reports per-row progress.
// OpenGL calls (load_image) must be made on the main thread after poll() returns true.
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

    // Returns true when the result is ready; moves it into `out`.
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
// App state shared with the GLFW drop callback
// ---------------------------------------------------------------------------

struct app_state {
    image_viewer*   single_viewer   = nullptr;
    compare_viewer* compare         = nullptr;
    image_data*     left_image      = nullptr;
    image_data*     right_image     = nullptr;
    bool*           compare_mode    = nullptr;
    bool*           pending_compare = nullptr;
    bool*           single_compare  = nullptr; // true: load same image to both panels
    std::string*    status_msg      = nullptr;
    async_loader*   left_loader     = nullptr;
    async_loader*   right_loader    = nullptr;
};

static void drop_callback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<app_state*>(glfwGetWindowUserPointer(window));

    if (count == 1) {
        app->left_loader->start(paths[0]);
        *app->pending_compare = *app->compare_mode;
        *app->single_compare  = *app->compare_mode; // same image on both panels
        *app->status_msg      = "Loading...";
    } else if (count >= 2) {
        app->left_loader->start(paths[0]);
        app->right_loader->start(paths[1]);
        *app->pending_compare = true;
        *app->single_compare  = false;
        *app->status_msg      = "Loading...";
    }
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) return 1;

    // OpenGL 3.3 Core
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
    glfwSwapInterval(1); // vsync
    glfwSetDropCallback(window, drop_callback);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // -------------------------------------------------------------------------
    // Application state
    // -------------------------------------------------------------------------
    image_viewer   single_viewer;
    compare_viewer compare;

    image_data left_image, right_image;

    char left_path_buf[512]  = "";
    char right_path_buf[512] = "";
    bool compare_mode        = false;
    bool pending_compare     = false;
    bool single_compare      = false;
    std::string status_msg;

    async_loader left_loader;
    async_loader right_loader;

    // Wire up drop callback state.
    app_state drop_state{&single_viewer, &compare,
                         &left_image, &right_image,
                         &compare_mode, &pending_compare, &single_compare,
                         &status_msg,
                         &left_loader, &right_loader};
    glfwSetWindowUserPointer(window, &drop_state);

    // Load images from command-line arguments (async).
    // 1 arg  → single viewer
    // 2 args → compare mode
    if (argc >= 3) {
        left_loader.start(argv[1]);
        right_loader.start(argv[2]);
        pending_compare = true;
        status_msg      = "Loading...";
    } else if (argc == 2) {
        left_loader.start(argv[1]);
        pending_compare = false;
        status_msg      = "Loading...";
    }

    // -------------------------------------------------------------------------
    // Main loop
    // -------------------------------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // fb_w/fb_h: physical pixels for glViewport
        // win_w/win_h: logical pixels (points) for ImGui layout
        int fb_w, fb_h, win_w, win_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glfwGetWindowSize(window, &win_w, &win_h);

        // ----- Poll async loaders (OpenGL calls must be on main thread) -----
        {
            image_data tmp;
            if (left_loader.poll(tmp)) {
                left_image = std::move(tmp);
                if (pending_compare) {
                    if (!right_loader.active) {
                        if (single_compare) {
                            // Split one image into left/right panels
                            compare.load_split(left_image);
                            compare.left_label  = left_loader.path;
                            compare.right_label = left_loader.path;
                            single_compare = false;
                        } else {
                            compare.load_left(left_image);
                            compare.left_label = left_loader.path;
                        }
                        compare_mode    = true;
                        pending_compare = false;
                        status_msg      = "Loaded: " + left_loader.path;
                    } else {
                        compare.load_left(left_image);
                        compare.left_label = left_loader.path;
                    }
                } else {
                    single_viewer.load_image(left_image);
                    compare_mode = false;
                    status_msg   = "Loaded: " + left_loader.path;
                }
            }
            if (right_loader.poll(tmp)) {
                right_image = std::move(tmp);
                compare.load_right(right_image);
                compare.right_label = right_loader.path;
                compare_mode        = true;
                pending_compare     = false;
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
        bool open_single  = false;
        bool open_compare = false;

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Image..."))   open_single  = true;
                if (ImGui::MenuItem("Open Compare...")) open_compare = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Compare Mode", nullptr, &compare_mode);
                ImGui::Separator();

                if (compare_mode) {
                    ImGui::MenuItem("Show Grid",    nullptr, &compare.show_grid);
                    ImGui::MenuItem("Show Minimap", nullptr, &compare.show_minimap);
                    ImGui::MenuItem("Sync Views",   nullptr, &compare.sync_views);
                    if (compare.is_split()) {
                        ImGui::Separator();
                        int split_x = compare.split_x;
                        ImGui::SetNextItemWidth(200.0f);
                        if (ImGui::SliderInt("Split##sp", &split_x, 1, compare.split_src_width() - 1))
                            compare.split_x = split_x;
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
                } else {
                    ImGui::MenuItem("Show Grid",    nullptr, &single_viewer.show_grid);
                    ImGui::MenuItem("Show Minimap", nullptr, &single_viewer.show_minimap);
                    if (single_viewer.show_grid) {
                        ImGui::Separator();
                        ImGui::SliderInt("Grid Spacing##s", &single_viewer.grid_spacing, 1, 500);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        if (open_single)  ImGui::OpenPopup("##open_single");
        if (open_compare) ImGui::OpenPopup("##open_compare");

        // ----- Open single image popup -----
        if (ImGui::BeginPopupModal("##open_single", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("TIFF path:");
            ImGui::SetNextItemWidth(400.0f);
            ImGui::InputText("##lp", left_path_buf, sizeof(left_path_buf));
            if (ImGui::Button("Load")) {
                left_loader.start(left_path_buf);
                pending_compare = false;
                status_msg      = "Loading...";
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ----- Open compare popup -----
        if (ImGui::BeginPopupModal("##open_compare", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Left TIFF:");
            ImGui::SetNextItemWidth(400.0f);
            ImGui::InputText("##lp2", left_path_buf, sizeof(left_path_buf));
            ImGui::Text("Right TIFF:");
            ImGui::SetNextItemWidth(400.0f);
            ImGui::InputText("##rp",  right_path_buf, sizeof(right_path_buf));
            if (ImGui::Button("Load")) {
                if (left_path_buf[0]) {
                    left_loader.start(left_path_buf);
                    if (right_path_buf[0]) {
                        right_loader.start(right_path_buf);
                        single_compare = false;
                    } else {
                        single_compare = true; // only left entered: split single image
                    }
                    pending_compare = true;
                    status_msg      = "Loading...";
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

        if (compare_mode) {
            compare.render(0.0f, viewer_h);
        } else {
            single_viewer.render("single_canvas", 0.0f, viewer_h);
        }

        // ----- Status bar -----
        ImGui::Separator();
        ImGui::TextUnformatted(status_msg.empty()
            ? "Ready  |  Drop TIFF to open  |  Drop 2 TIFFs to compare  |  Scroll: zoom  |  Ctrl+Scroll: pan H  |  Shift+Scroll: pan V  |  Drag: pan  |  Double-click: fit"
            : status_msg.c_str());

        ImGui::End();

        // ----- Loading progress overlay (rendered after main window to appear on top) -----
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
