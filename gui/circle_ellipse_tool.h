#pragma once
#include "util/analysis_tool.h"
#include <vector>
#include <string>

// One detected circle or ellipse.
struct detected_shape {
    enum class kind_t { circle, ellipse };

    kind_t kind;
    float  cx, cy;       // center in image coordinates
    float  rx, ry;       // semi-axes: rx is along `angle_deg`, ry is perpendicular
    float  angle_deg;    // rotation of rx axis from positive x-axis (degrees, CW = OpenCV)

    float major_semi()     const { return rx >= ry ? rx : ry; }
    float minor_semi()     const { return rx >= ry ? ry : rx; }
    float major_diameter() const { return major_semi() * 2.0f; }
    float minor_diameter() const { return minor_semi() * 2.0f; }
    float axis_ratio()     const { return major_semi() > 0.0f ? minor_semi() / major_semi() : 0.0f; }
};

class circle_ellipse_tool : public analysis_tool {
public:
    // ----- Detection parameters (editable in UI) -----
    float canny_t1        = 50.0f;
    float canny_t2        = 150.0f;
    float min_radius      = 10.0f;
    float max_radius      = 500.0f;
    float hough_dp        = 1.5f;
    float hough_min_dist  = 50.0f;
    float hough_param2    = 30.0f;   // HoughCircles accumulator threshold
    float min_axis_ratio  = 0.5f;    // reject shapes whose minor/major < this
    int   min_contour_px  = 300;     // min contour area in pixels
    bool  detect_circles  = true;
    bool  detect_ellipses = true;

    std::string_view name() const override { return "Circle/Ellipse"; }

    void analyze(const image_data& img) override;
    void render_panel() override;
    void render_overlay(ImDrawList* dl,
                        ImVec2      canvas_pos,
                        ImVec2      canvas_size,
                        float       zoom,
                        float       pan_x,
                        float       pan_y) override;

    const std::vector<detected_shape>& results() const { return shapes_; }
    bool has_results() const { return !shapes_.empty(); }
    void clear_results() { shapes_.clear(); }

private:
    std::vector<detected_shape> shapes_;

    static void draw_rotated_ellipse(ImDrawList* dl, ImVec2 center,
                                     float rx, float ry, float angle_deg,
                                     ImU32 col, float thickness);
};
