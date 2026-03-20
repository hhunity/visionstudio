#include "gui/image_viewer.h"
#include <glad/glad.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

image_viewer::image_viewer()  = default;
image_viewer::~image_viewer() { destroy_texture(); }

bool image_viewer::load_image(const image_data& img) {
    if (img.empty()) return false;
    destroy_texture();
    create_texture(img);
    img_w_       = img.width;
    img_h_       = img.height;
    cpu_image_   = img;          // keep CPU copy for pixel inspection
    owned_state_ = view_state{};
    needs_fit_   = true;
    return texture_id_ != 0;
}

void image_viewer::unload_image() {
    destroy_texture();
    img_w_ = img_h_ = 0;
}

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------

void image_viewer::create_texture(const image_data& img) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); // crisp pixels when zoomed in
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 img.width, img.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img.pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    texture_id_ = static_cast<uint32_t>(tex);
}

void image_viewer::destroy_texture() {
    if (texture_id_ != 0) {
        GLuint tex = static_cast<GLuint>(texture_id_);
        glDeleteTextures(1, &tex);
        texture_id_ = 0;
    }
}

// ---------------------------------------------------------------------------
// View helpers
// ---------------------------------------------------------------------------

void image_viewer::fit_view(view_state& state, float canvas_w, float canvas_h) const {
    if (img_w_ == 0 || img_h_ == 0) return;
    const float zoom_x = canvas_w / static_cast<float>(img_w_);
    const float zoom_y = canvas_h / static_cast<float>(img_h_);
    state.zoom  = std::min(zoom_x, zoom_y);
    state.pan_x = (canvas_w - img_w_ * state.zoom) * 0.5f;
    state.pan_y = (canvas_h - img_h_ * state.zoom) * 0.5f;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void image_viewer::render(const char* id, float width, float height,
                          view_state* ext_state) {
    view_state* state = ext_state ? ext_state : &owned_state_;

    ImVec2 canvas_pos  = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = {width, height};
    if (canvas_size.x <= 0.0f) canvas_size.x = ImGui::GetContentRegionAvail().x;
    if (canvas_size.y <= 0.0f) canvas_size.y = ImGui::GetContentRegionAvail().y;
    if (canvas_size.x < 1.0f)  canvas_size.x = 1.0f;
    if (canvas_size.y < 1.0f)  canvas_size.y = 1.0f;

    // Auto-fit on the first render after a new image is loaded.
    if (needs_fit_) {
        fit_view(*state, canvas_size.x, canvas_size.y);
        needs_fit_ = false;
    }

    // Invisible button covers the whole canvas to capture mouse input.
    ImGui::InvisibleButton(id, canvas_size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    handle_input(canvas_pos, canvas_size, *state);
    (void)active; // input handling checks internally

    // Draw
    ImDrawList* dl      = ImGui::GetWindowDrawList();
    const ImVec2 clip_max = {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y};
    dl->PushClipRect(canvas_pos, clip_max, true);
    draw_content(dl, canvas_pos, canvas_size, *state);
    dl->PopClipRect();

    if (show_coordinates && hovered && texture_id_ != 0)
        draw_coordinate_tooltip(canvas_pos, *state);
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

void image_viewer::handle_input(const ImVec2& canvas_pos, const ImVec2& canvas_size,
                                 view_state& state) const {
    // Pan with left mouse drag
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        state.pan_x += delta.x;
        state.pan_y += delta.y;
    }

    // Scroll wheel: zoom / pan depending on modifier keys
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const bool ctrl  = ImGui::GetIO().KeyCtrl;
            const bool shift = ImGui::GetIO().KeyShift;
            constexpr float pan_speed = 32.0f; // pixels per scroll step
            if (ctrl) {
                // Ctrl + scroll → horizontal pan
                state.pan_x += wheel * pan_speed;
            } else if (shift) {
                // Shift + scroll → vertical pan
                state.pan_y += wheel * pan_speed;
            } else {
                // Plain scroll → zoom toward cursor
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const float rel_x  = mouse.x - canvas_pos.x;
                const float rel_y  = mouse.y - canvas_pos.y;
                const float img_x  = (rel_x - state.pan_x) / state.zoom;
                const float img_y  = (rel_y - state.pan_y) / state.zoom;
                const float factor = (wheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
                state.zoom  = std::min(std::max(state.zoom * factor, 0.001f), 500.0f);
                state.pan_x = rel_x - img_x * state.zoom;
                state.pan_y = rel_y - img_y * state.zoom;
            }
        }

        // Double-click: fit image to canvas
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            fit_view(state, canvas_size.x, canvas_size.y);
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void image_viewer::draw_content(ImDrawList* dl, const ImVec2& canvas_pos,
                                 const ImVec2& canvas_size,
                                 const view_state& state) const {
    const ImVec2 clip_max = {canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y};

    // Background
    dl->AddRectFilled(canvas_pos, clip_max, IM_COL32(40, 40, 40, 255));

    if (texture_id_ == 0) {
        dl->AddText({canvas_pos.x + 10.0f, canvas_pos.y + 10.0f},
                    IM_COL32(180, 180, 180, 255), "No image loaded");
        return;
    }

    const float img_sx = canvas_pos.x + state.pan_x;
    const float img_sy = canvas_pos.y + state.pan_y;
    const float img_sw = img_w_ * state.zoom;
    const float img_sh = img_h_ * state.zoom;

    // Cast texture id to the void* ImTextureID expected by ImGui
    auto tex_id = static_cast<ImTextureID>(texture_id_);
    dl->AddImage(tex_id, {img_sx, img_sy}, {img_sx + img_sw, img_sy + img_sh});

    if (show_grid)
        draw_grid(dl, canvas_pos, canvas_size, state);

    if (show_minimap)
        draw_minimap(dl, canvas_pos, canvas_size, state);
}

void image_viewer::draw_grid(ImDrawList* dl, const ImVec2& origin,
                              const ImVec2& size, const view_state& state) const {
    const float spacing_px = grid_spacing * state.zoom; // grid spacing in screen pixels
    if (spacing_px < 4.0f) return;                      // too dense to be useful

    constexpr ImU32 grid_color = IM_COL32(100, 100, 255, 100);
    const float end_x = origin.x + size.x;
    const float end_y = origin.y + size.y;

    // Vertical lines
    float sx = std::fmod(state.pan_x, spacing_px);
    if (sx < 0.0f) sx += spacing_px;
    for (float x = sx; x <= size.x; x += spacing_px)
        dl->AddLine({origin.x + x, origin.y}, {origin.x + x, end_y}, grid_color);

    // Horizontal lines
    float sy = std::fmod(state.pan_y, spacing_px);
    if (sy < 0.0f) sy += spacing_px;
    for (float y = sy; y <= size.y; y += spacing_px)
        dl->AddLine({origin.x, origin.y + y}, {end_x, origin.y + y}, grid_color);
}

void image_viewer::draw_coordinate_tooltip(const ImVec2& canvas_pos,
                                            const view_state& state) const {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float img_x  = (mouse.x - canvas_pos.x - state.pan_x) / state.zoom;
    const float img_y  = (mouse.y - canvas_pos.y - state.pan_y) / state.zoom;
    if (img_x < 0.0f || img_x >= img_w_ || img_y < 0.0f || img_y >= img_h_) return;

    const int px = static_cast<int>(img_x);
    const int py = static_cast<int>(img_y);
    const auto rgba = cpu_image_.pixel_at(px, py);

    ImGui::BeginTooltip();
    ImGui::Text("pos  : (%d, %d)", px, py);
    ImGui::Text("zoom : %.2fx", static_cast<double>(state.zoom));
    ImGui::Separator();
    ImGui::ColorButton("##swatch",
        ImVec4(rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f),
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, {16, 16});
    ImGui::SameLine();
    ImGui::Text("R:%3d  G:%3d  B:%3d  A:%3d", rgba[0], rgba[1], rgba[2], rgba[3]);
    ImGui::EndTooltip();
}

image_viewer::mouse_query image_viewer::query_mouse_pixel(
        const ImVec2& canvas_pos, const ImVec2& canvas_size,
        const view_state& state) const {
    mouse_query q;
    if (texture_id_ == 0) return q;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x < canvas_pos.x || mouse.x >= canvas_pos.x + canvas_size.x ||
        mouse.y < canvas_pos.y || mouse.y >= canvas_pos.y + canvas_size.y)
        return q;
    const float fx = (mouse.x - canvas_pos.x - state.pan_x) / state.zoom;
    const float fy = (mouse.y - canvas_pos.y - state.pan_y) / state.zoom;
    if (fx < 0.0f || fx >= img_w_ || fy < 0.0f || fy >= img_h_) return q;
    q.valid = true;
    q.img_x = static_cast<int>(fx);
    q.img_y = static_cast<int>(fy);
    q.rgba  = cpu_image_.pixel_at(q.img_x, q.img_y);
    return q;
}

std::array<uint8_t, 4> image_viewer::pixel_at(int x, int y) const {
    return cpu_image_.pixel_at(x, y);
}

void image_viewer::draw_minimap(ImDrawList* dl, const ImVec2& canvas_pos,
                                 const ImVec2& canvas_size,
                                 const view_state& state) const {
    constexpr float max_w  = 160.0f;
    constexpr float max_h  = 120.0f;
    constexpr float margin =   8.0f;

    // Preserve image aspect ratio within the max bounds.
    const float aspect = static_cast<float>(img_w_) / static_cast<float>(img_h_);
    float mw, mh;
    if (aspect >= max_w / max_h) { mw = max_w; mh = max_w / aspect; }
    else                          { mh = max_h; mw = max_h * aspect; }

    // Top-right corner of canvas.
    const float mx = canvas_pos.x + canvas_size.x - mw - margin;
    const float my = canvas_pos.y + margin;

    // Semi-transparent background.
    dl->AddRectFilled({mx - 1.0f, my - 1.0f}, {mx + mw + 1.0f, my + mh + 1.0f},
                      IM_COL32(0, 0, 0, 160));

    // Thumbnail — same texture, full UV range.
    auto tex_id = static_cast<ImTextureID>(texture_id_);
    dl->AddImage(tex_id, {mx, my}, {mx + mw, my + mh});

    // Visible region in image space.
    const float vis_x0 = -state.pan_x / state.zoom;
    const float vis_y0 = -state.pan_y / state.zoom;
    const float vis_x1 = (-state.pan_x + canvas_size.x) / state.zoom;
    const float vis_y1 = (-state.pan_y + canvas_size.y) / state.zoom;

    // Map to minimap space and clamp to minimap bounds.
    const float sx = mw / static_cast<float>(img_w_);
    const float sy = mh / static_cast<float>(img_h_);
    const float rx0 = mx + std::max(0.0f, std::min(mw, vis_x0 * sx));
    const float ry0 = my + std::max(0.0f, std::min(mh, vis_y0 * sy));
    const float rx1 = mx + std::max(0.0f, std::min(mw, vis_x1 * sx));
    const float ry1 = my + std::max(0.0f, std::min(mh, vis_y1 * sy));

    // Red viewport rectangle.
    dl->AddRect({rx0, ry0}, {rx1, ry1}, IM_COL32(255, 50, 50, 230), 0.0f, 0, 1.5f);

    // Minimap border.
    dl->AddRect({mx - 1.0f, my - 1.0f}, {mx + mw + 1.0f, my + mh + 1.0f},
                IM_COL32(120, 120, 120, 200));
}
