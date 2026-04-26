#include "rendering/render/layer_depth_bins.hpp"

#include <algorithm>

namespace render_depth {

std::vector<double> build_linear_depth_edges(double max_depth, double base_layer_interval) {
    std::vector<double> edges;
    edges.reserve(static_cast<std::size_t>(kMaxLayersPerSide + 1));
    edges.push_back(0.0);
    while (edges.back() < max_depth && static_cast<int>(edges.size()) - 1 < kMaxLayersPerSide) {
        const double distance = edges.back();
        const double next_distance = std::min(max_depth, distance + base_layer_interval);
        if (next_distance <= distance) {
            break;
        }
        edges.push_back(next_distance);
    }
    if (edges.size() == 1) {
        edges.push_back(max_depth);
    } else if (edges.back() < max_depth) {
        edges.back() = max_depth;
    }
    return edges;
}

std::vector<double> build_background_depth_edges(double max_depth,
                                                 double base_layer_interval,
                                                 double depth_curve) {
    std::vector<double> edges;
    edges.reserve(static_cast<std::size_t>(kMaxLayersPerSide + 1));
    edges.push_back(0.0);
    while (edges.back() < max_depth && static_cast<int>(edges.size()) - 1 < kMaxLayersPerSide) {
        const double distance = edges.back();
        const double t = std::clamp(distance / std::max(1.0, max_depth), 0.0, 1.0);
        const double growth = 1.0 + depth_curve * t * t;
        const double step = std::max(1.0, base_layer_interval * growth);
        const double next_distance = std::min(max_depth, distance + step);
        if (next_distance <= distance) {
            break;
        }
        edges.push_back(next_distance);
    }
    if (edges.size() == 1) {
        edges.push_back(max_depth);
    } else if (edges.back() < max_depth) {
        edges.back() = max_depth;
    }
    return edges;
}

}
