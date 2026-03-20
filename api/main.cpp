#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "gui/compare_viewer.h"
#include "gui/image_viewer.h"
#include "io/tiff_io.h"
#include "util/image_data.h"

#include <cstdio>
#include <string>

static void glfw_error_cb(int error, const char* desc) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

// State shared between the main loop and the GLFW drop callback.
struct app_state {
    image_viewer*   single_viewer = nullptr;
    compare_viewer* compare       = nullptr;
    image_data*     left_image    = nullptr;
    image_data*     right_image   = nullptr;
    bool*           compare_mode  = nullptr;
    std::string*    status_msg    = nullptr;
};

static void drop_callback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<app_state*>(glfwGetWindowUserPointer(window));

    if (count == 1) {
        image_data img;
        if (tiff_io::read(paths[0], img)) {
            *app->left_image = std::move(img);
            if (*app->compare_mode) {
                app->compare->load_left(*app->left_image);
                app->compare->left_label = paths[0];
            } else {
                app->single_viewer->load_image(*app->left_image);
            }
            *app->status_msg = std::string("Loaded: ") + paths[0];
        } else {
            *app->status_msg = std::string("Failed to load: ") + paths[0];
        }
    } else if (count >= 2) {
        // Two or more files dropped — switch to compare mode automatically.
        bool ok_l = tiff_io::read(paths[0], *app->left_image);
        bool ok_r = tiff_io::read(paths[1], *app->right_image);
        if (ok_l) { app->compare->load_left(*app->left_image);  app->compare->left_label  = paths[0]; }
        if (ok_r) { app->compare->load_right(*app->right_image); app->compare->right_label = paths[1]; }
        *app->compare_mode = true;
        *app->status_msg   = (ok_l && ok_r) ? "Compare: both images loaded"
                                             : "Compare: one or more images failed to load";
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
    std::string status_msg;

    // Wire up drop callback state.
    app_state drop_state{&single_viewer, &compare,
                         &left_image, &right_image,
                         &compare_mode, &status_msg};
    glfwSetWindowUserPointer(window, &drop_state);

    // Load images from command-line arguments.
    // 1 arg  → single viewer
    // 2 args → compare mode
    if (argc >= 3) {
        const bool ok_l = tiff_io::read(argv[1], left_image);
        const bool ok_r = tiff_io::read(argv[2], right_image);
        if (ok_l) { compare.load_left(left_image);   compare.left_label  = argv[1]; }
        if (ok_r) { compare.load_right(right_image); compare.right_label = argv[2]; }
        compare_mode = true;
        status_msg   = (ok_l && ok_r) ? "Compare: both images loaded"
                                      : "Compare: one or more images failed to load";
    } else if (argc == 2) {
        if (tiff_io::read(argv[1], left_image)) {
            single_viewer.load_image(left_image);
            status_msg = std::string("Loaded: ") + argv[1];
        } else {
            status_msg = std::string("Failed to load: ") + argv[1];
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

        // fb_w/fb_h: physical pixels for glViewport
        // win_w/win_h: logical pixels (points) for ImGui layout
        int fb_w, fb_h, win_w, win_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glfwGetWindowSize(window, &win_w, &win_h);

        // Full-screen host window
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({static_cast<float>(win_w), static_cast<float>(win_h)});
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_MenuBar  |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ----- Menu bar -----
        // Use flags to defer OpenPopup calls outside of the menu scope,
        // because OpenPopup inside BeginMenu/EndMenu uses a different ID stack.
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
                    ImGui::Separator();
                    if (ImGui::MenuItem("Diff Mode", nullptr, &compare.diff_mode))
                        compare.diff_amplify = 1.0f; // reset amplify on toggle
                    if (compare.diff_mode)
                        ImGui::SliderFloat("Amplify##d", &compare.diff_amplify, 1.0f, 20.0f);
                    ImGui::Separator();
                    ImGui::SliderInt("Grid Spacing##c", &compare.grid_spacing, 10, 500);
                } else {
                    ImGui::MenuItem("Show Grid",    nullptr, &single_viewer.show_grid);
                    ImGui::MenuItem("Show Minimap", nullptr, &single_viewer.show_minimap);
                    ImGui::SliderInt("Grid Spacing##s", &single_viewer.grid_spacing, 10, 500);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Open popups here, outside the menu scope, so the ID stack is consistent.
        if (open_single)  ImGui::OpenPopup("##open_single");
        if (open_compare) ImGui::OpenPopup("##open_compare");

        // ----- Open single image popup -----
        if (ImGui::BeginPopupModal("##open_single", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("TIFF path:");
            ImGui::SetNextItemWidth(400.0f);
            ImGui::InputText("##lp", left_path_buf, sizeof(left_path_buf));
            if (ImGui::Button("Load")) {
                image_data img;
                if (tiff_io::read(left_path_buf, img)) {
                    left_image = std::move(img);
                    single_viewer.load_image(left_image);
                    compare_mode      = false;
                    compare.left_label = left_path_buf;
                    status_msg        = std::string("Loaded: ") + left_path_buf;
                } else {
                    status_msg = std::string("Failed to load: ") + left_path_buf;
                }
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
                bool ok_l = false, ok_r = false;
                image_data limg, rimg;
                if ((ok_l = tiff_io::read(left_path_buf,  limg))) {
                    left_image = std::move(limg);
                    compare.load_left(left_image);
                    compare.left_label = left_path_buf;
                }
                if ((ok_r = tiff_io::read(right_path_buf, rimg))) {
                    right_image = std::move(rimg);
                    compare.load_right(right_image);
                    compare.right_label = right_path_buf;
                }
                compare_mode = true;
                status_msg   = (ok_l && ok_r) ? "Both images loaded"
                                               : "One or more images failed to load";
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
