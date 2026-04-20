#include "rendering/render/layer_submission_builder.hpp"

#include "rendering/render/render.hpp"
#include "rendering/render/warped_screen_grid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxDofLayers = 512;
constexpr int kMaxLayersPerSide = kMaxDofLayers / 2;

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

int depth_to_layer_index(double depth,
                         double max_cull_depth,
                         const std::vector<double>& foreground_depth_edges,
                         const std::vector<double>& background_depth_edges,
                         int foreground_layer_count,
                         int background_layer_count,
                         int total_layer_count) {
    if (!std::isfinite(depth) || total_layer_count <= 0) {
        return -1;
    }

    const double clamped = std::clamp(depth, -max_cull_depth, max_cull_depth);
    const double abs_depth = std::fabs(clamped);
    if (clamped <= 0.0) {
        auto upper = std::upper_bound(foreground_depth_edges.begin(), foreground_depth_edges.end(), abs_depth);
        std::ptrdiff_t seg = std::distance(foreground_depth_edges.begin(), upper) - 1;
        seg = std::clamp<std::ptrdiff_t>(seg, 0, static_cast<std::ptrdiff_t>(foreground_layer_count - 1));
        const int idx = foreground_layer_count - 1 - static_cast<int>(seg);
        return std::clamp(idx, 0, total_layer_count - 1);
    }

    auto upper = std::upper_bound(background_depth_edges.begin(), background_depth_edges.end(), abs_depth);
    std::ptrdiff_t seg = std::distance(background_depth_edges.begin(), upper) - 1;
    seg = std::clamp<std::ptrdiff_t>(seg, 0, static_cast<std::ptrdiff_t>(background_layer_count - 1));
    const int idx = foreground_layer_count + static_cast<int>(seg);
    return std::clamp(idx, 0, total_layer_count - 1);
}

std::pair<double, double> layer_slice_depth_bounds(int layer_idx,
                                                    int foreground_layer_count,
                                                    int total_layer_count,
                                                    const std::vector<double>& foreground_depth_edges,
                                                    const std::vector<double>& background_depth_edges) {
    const int clamped_idx = std::clamp(layer_idx, 0, total_layer_count - 1);
    if (clamped_idx < foreground_layer_count) {
        const int seg = foreground_layer_count - 1 - clamped_idx;
        const double low_abs = foreground_depth_edges[static_cast<std::size_t>(seg)];
        const double high_abs = foreground_depth_edges[static_cast<std::size_t>(seg + 1)];
        return std::pair<double, double>{-high_abs, -low_abs};
    }
    const int seg = clamped_idx - foreground_layer_count;
    const double low_abs = background_depth_edges[static_cast<std::size_t>(seg)];
    const double high_abs = background_depth_edges[static_cast<std::size_t>(seg + 1)];
    return std::pair<double, double>{low_abs, high_abs};
}

void expand_layer_bounds(render_pipeline::LayerSubmission& layer,
                         const std::array<SDL_Vertex, 4>& vertices) {
    for (const SDL_Vertex& vertex : vertices) {
        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y)) {
            continue;
        }
        layer.bounds_min_x = std::min(layer.bounds_min_x, vertex.position.x);
        layer.bounds_min_y = std::min(layer.bounds_min_y, vertex.position.y);
        layer.bounds_max_x = std::max(layer.bounds_max_x, vertex.position.x);
        layer.bounds_max_y = std::max(layer.bounds_max_y, vertex.position.y);
    }
}

} // namespace

render_pipeline::LayerBuildResult LayerSubmissionBuilder::build(const GeometryBatcher& geometry_batcher,
                                                                const WarpedScreenGrid& cam,
                                                                double player_split_world_z,
                                                                double max_cull_depth) const {
    render_pipeline::LayerBuildResult result{};
    const auto realism = cam.get_settings();
    const double safe_max_cull_depth = std::max(1.0, max_cull_depth);
    const double base_layer_interval = std::max(1.0, static_cast<double>(realism.layer_depth_interval));
    const double depth_curve = std::max(0.0, static_cast<double>(realism.layer_depth_curve));

    const std::vector<double> foreground_depth_edges =
        build_linear_depth_edges(safe_max_cull_depth, base_layer_interval);
    const std::vector<double> background_depth_edges =
        build_background_depth_edges(safe_max_cull_depth, base_layer_interval, depth_curve);

    const int foreground_layer_count = std::max(1, static_cast<int>(foreground_depth_edges.size()) - 1);
    const int background_layer_count = std::max(1, static_cast<int>(background_depth_edges.size()) - 1);
    const int layer_count = foreground_layer_count + background_layer_count;

    result.layer_count = layer_count;
    result.layers.resize(static_cast<std::size_t>(layer_count));
    for (int i = 0; i < layer_count; ++i) {
        auto& layer = result.layers[static_cast<std::size_t>(i)];
        const auto [slice_depth_min, slice_depth_max] = layer_slice_depth_bounds(i,
                                                                                  foreground_layer_count,
                                                                                  layer_count,
                                                                                  foreground_depth_edges,
                                                                                  background_depth_edges);
        layer.slice_depth_min = slice_depth_min;
        layer.slice_depth_max = slice_depth_max;
        layer.slice_reference_depth = 0.5 * (slice_depth_min + slice_depth_max);
        layer.representative_depth = layer.slice_reference_depth;
    }

    const int center_split_layer_index = std::clamp(
        depth_to_layer_index(0.0,
                             safe_max_cull_depth,
                             foreground_depth_edges,
                             background_depth_edges,
                             foreground_layer_count,
                             background_layer_count,
                             layer_count),
        0,
        std::max(0, layer_count - 1));

    double player_layer_depth_from_anchor = 0.0;
    if (std::isfinite(player_split_world_z)) {
        const double anchor_world_z = cam.anchor_world_z();
        if (std::isfinite(anchor_world_z)) {
            player_layer_depth_from_anchor = anchor_world_z - player_split_world_z;
        }
    }
    if (!std::isfinite(player_layer_depth_from_anchor)) {
        player_layer_depth_from_anchor = 0.0;
    }

    int resolved_player_layer_index = depth_to_layer_index(player_layer_depth_from_anchor,
                                                           safe_max_cull_depth,
                                                           foreground_depth_edges,
                                                           background_depth_edges,
                                                           foreground_layer_count,
                                                           background_layer_count,
                                                           layer_count);
    if (resolved_player_layer_index < 0 || resolved_player_layer_index >= layer_count) {
        resolved_player_layer_index = center_split_layer_index;
    }
    result.player_layer_index = resolved_player_layer_index;

    geometry_batcher.for_each_item_far_to_near([&](const GeometryBatcher::DrawItem& item) {
        const int layer_idx = depth_to_layer_index(item.depth,
                                                   safe_max_cull_depth,
                                                   foreground_depth_edges,
                                                   background_depth_edges,
                                                   foreground_layer_count,
                                                   background_layer_count,
                                                   layer_count);
        if (layer_idx < 0 || layer_idx >= layer_count) {
            return;
        }

        auto& layer = result.layers[static_cast<std::size_t>(layer_idx)];
        if (layer.draws.empty()) {
            const double midpoint = layer.slice_reference_depth;
            layer.depth_min = std::isfinite(item.depth) ? item.depth : midpoint;
            layer.depth_max = std::isfinite(item.depth) ? item.depth : midpoint;
            layer.bounds_min_x = std::numeric_limits<float>::infinity();
            layer.bounds_min_y = std::numeric_limits<float>::infinity();
            layer.bounds_max_x = -std::numeric_limits<float>::infinity();
            layer.bounds_max_y = -std::numeric_limits<float>::infinity();
        }

        render_pipeline::GeometryLayerDrawItem copy{};
        copy.texture = item.texture;
        copy.blend_mode = item.blend_mode;
        copy.vertices[0] = item.vertices[0];
        copy.vertices[1] = item.vertices[1];
        copy.vertices[2] = item.vertices[2];
        copy.vertices[3] = item.vertices[3];
        copy.depth = item.depth;
        layer.draws.push_back(copy);

        if (std::isfinite(item.depth)) {
            layer.depth_min = std::min(layer.depth_min, item.depth);
            layer.depth_max = std::max(layer.depth_max, item.depth);
        }
        expand_layer_bounds(layer, copy.vertices);
    });

    for (int i = 0; i < layer_count; ++i) {
        auto& layer = result.layers[static_cast<std::size_t>(i)];
        if (layer.draws.empty()) {
            continue;
        }
        if (!std::isfinite(layer.depth_min) || !std::isfinite(layer.depth_max)) {
            layer.depth_min = layer.slice_reference_depth;
            layer.depth_max = layer.slice_reference_depth;
        }
        if (!std::isfinite(layer.representative_depth)) {
            layer.representative_depth = 0.5 * (layer.depth_min + layer.depth_max);
        }
        if (!std::isfinite(layer.bounds_min_x) || !std::isfinite(layer.bounds_min_y) ||
            !std::isfinite(layer.bounds_max_x) || !std::isfinite(layer.bounds_max_y)) {
            layer.bounds_min_x = 0.0f;
            layer.bounds_min_y = 0.0f;
            layer.bounds_max_x = 0.0f;
            layer.bounds_max_y = 0.0f;
        }
        result.non_empty_layers.push_back(i);
    }

    result.valid = !result.non_empty_layers.empty();
    return result;
}
