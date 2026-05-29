#pragma once

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace render_depth {

constexpr int kMaxDofLayers = 512;
constexpr int kMaxLayersPerSide = kMaxDofLayers / 2;
constexpr int kDofFocusBucketRadius = 2;

inline float dof_blur_strength_for_layer_distance(int layer,
                                                  int focus_layer,
                                                  int max_distance_from_focus = kDofFocusBucketRadius) {
    const int distance_from_focus = std::abs(layer - focus_layer);
    if (distance_from_focus == 0 || max_distance_from_focus <= 0) {
        return 0.0f;
    }

    const float normalized_distance = std::clamp(
        static_cast<float>(distance_from_focus) / static_cast<float>(max_distance_from_focus),
        0.0f,
        1.0f);
    return normalized_distance * normalized_distance * (3.0f - 2.0f * normalized_distance);
}

std::vector<double> build_linear_depth_edges(double max_depth, double base_layer_interval);
std::vector<double> build_background_depth_edges(double max_depth,
                                                 double base_layer_interval,
                                                 double depth_curve);

}
