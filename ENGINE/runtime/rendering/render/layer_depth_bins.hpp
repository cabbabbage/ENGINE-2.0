#pragma once

#include <vector>

namespace render_depth {

constexpr int kMaxDofLayers = 512;
constexpr int kMaxLayersPerSide = kMaxDofLayers / 2;

std::vector<double> build_linear_depth_edges(double max_depth, double base_layer_interval);
std::vector<double> build_background_depth_edges(double max_depth,
                                                 double base_layer_interval,
                                                 double depth_curve);

}
