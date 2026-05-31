#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace render_depth {

constexpr int kMaxDofLayers = 512;
constexpr int kMaxLayersPerSide = kMaxDofLayers / 2;
constexpr int kDofFocusBucketRadius = 2;

struct LensBlurDepthSettings {
    float focus_depth_offset = 0.0f;
    float aperture = 1.0f;
    float focus_falloff_acceleration = 1.5f;
    float max_near_blur_px = 16.0f;
    float max_far_blur_px = 48.0f;
    float near_far_blur_bias = 0.0f;
    float focus_dead_zone = 1.0e-3f;
};

inline float dof_blur_amount_for_layer_depth(float layer_depth,
                                             float focus_depth,
                                             const LensBlurDepthSettings& settings) {
    const float safe_focus_depth = std::isfinite(focus_depth) ? focus_depth : 0.0f;
    const float safe_layer_depth = std::isfinite(layer_depth) ? layer_depth : safe_focus_depth;
    const float safe_focus_offset = std::isfinite(settings.focus_depth_offset) ? settings.focus_depth_offset : 0.0f;
    const float signed_delta = safe_layer_depth - (safe_focus_depth + safe_focus_offset);
    const float delta = std::abs(signed_delta);
    const float focus_dead_zone = std::max(0.0f, settings.focus_dead_zone);
    if (delta <= focus_dead_zone) {
        return 0.0f;
    }

    const bool foreground_layer = signed_delta < 0.0f;
    const float safe_aperture = std::max(0.0f, std::isfinite(settings.aperture) ? settings.aperture : 0.0f);
    const float safe_acceleration = std::max(
        0.01f,
        std::isfinite(settings.focus_falloff_acceleration) ? settings.focus_falloff_acceleration : 1.0f);
    const float max_near = std::max(0.0f, std::isfinite(settings.max_near_blur_px) ? settings.max_near_blur_px : 0.0f);
    const float max_far = std::max(0.0f, std::isfinite(settings.max_far_blur_px) ? settings.max_far_blur_px : 0.0f);
    const float bias = std::clamp(std::isfinite(settings.near_far_blur_bias) ? settings.near_far_blur_bias : 0.0f,
                                  -1.0f,
                                  1.0f);

    float max_blur = foreground_layer ? max_near : max_far;
    max_blur *= foreground_layer ? (1.0f - bias) : (1.0f + bias);
    max_blur = std::max(0.0f, max_blur);
    if (max_blur <= 0.0f || safe_aperture <= 0.0f) {
        return 0.0f;
    }

    const float falloff = std::pow(delta * safe_aperture, safe_acceleration);
    return std::clamp(falloff, 0.0f, max_blur);
}

std::vector<double> build_linear_depth_edges(double max_depth, double base_layer_interval);
std::vector<double> build_background_depth_edges(double max_depth,
                                                 double base_layer_interval,
                                                 double depth_curve);

}
