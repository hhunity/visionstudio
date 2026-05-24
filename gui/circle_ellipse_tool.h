#pragma once
#include "util/analysis_tool.h"
#include <opencv2/core.hpp>
#include <atomic>
#include <future>
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
    float max_fit_error   = 0.2f;    // max mean distance from fitted ellipse (normalized)
    int   min_contour_px  = 300;     // min contour area in pixels
    bool  detect_circles  = true;
    bool  detect_ellipses = true;
    int   max_detect_size = 1024;    // downsample longest side to this before detection

    std::string_view name() const override { return "Circle/Ellipse"; }

    // Synchronous (called by async task internally).
    void analyze(const image_data& img) override;

    // Launch detection in background. Ignored if already running.
    void start_analyze(const image_data& img);
    bool is_analyzing() const;

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
    std::vector<detected_shape>              shapes_;
    std::future<std::vector<detected_shape>> analyze_future_;
    std::atomic<float>                       analyze_progress_{0.0f};
    std::atomic<bool>                        cancel_requested_{false};
    bool                                     last_cancelled_{false};
    bool                                     show_graph_{false};

    // Run detection with explicit params (safe to call from background thread).
    // progress: updated 0.0→1.0 as work proceeds; nullable.
    static std::vector<detected_shape> run_detection(
        const image_data& img,
        float canny_t1, float canny_t2,
        float min_radius, float max_radius,
        float hough_dp, float hough_min_dist, float hough_param2,
        float min_axis_ratio, int min_contour_px,
        bool detect_circles, bool detect_ellipses,
        int max_detect_size, float max_fit_error,
        std::atomic<float>* progress,
        std::atomic<bool>*  cancel);

    static float ellipse_fit_error(const std::vector<cv::Point>& contour,
                                   const cv::RotatedRect& ell);

    static void draw_rotated_ellipse(ImDrawList* dl, ImVec2 center,
                                     float rx, float ry, float angle_deg,
                                     ImU32 col, float thickness);
};

