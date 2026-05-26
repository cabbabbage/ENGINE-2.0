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
                                                  int bucket_radius = kDofFocusBucketRadius) {
    if (bucket_radius <= 0) {
        return layer == focus_layer ? 0.0f : 1.0f;
    }

    const float normalized_distance = std::clamp(
        static_cast<float>(std::abs(layer - focus_layer)) / static_cast<float>(bucket_radius),
        0.0f,
        1.0f);
    return normalized_distance * normalized_distance * normalized_distance;
}

std::vector<double> build_linear_depth_edges(double max_depth, double base_layer_interval);
std::vector<double> build_background_depth_edges(double max_depth,
                                                 double base_layer_interval,
                                                 double depth_curve);

}
