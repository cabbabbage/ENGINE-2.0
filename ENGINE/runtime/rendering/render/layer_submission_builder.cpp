#include "rendering/render/layer_submission_builder.hpp"

#include "rendering/render/layer_depth_bins.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/warped_screen_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {

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

double layer_midpoint_depth(int layer_idx,
                            int foreground_layer_count,
                            int total_layer_count,
                            const std::vector<double>& foreground_depth_edges,
                            const std::vector<double>& background_depth_edges) {
    const int clamped_idx = std::clamp(layer_idx, 0, total_layer_count - 1);
    if (clamped_idx < foreground_layer_count) {
        const int seg = foreground_layer_count - 1 - clamped_idx;
        const double low_abs = foreground_depth_edges[static_cast<std::size_t>(seg)];
        const double high_abs = foreground_depth_edges[static_cast<std::size_t>(seg + 1)];
        return -0.5 * (low_abs + high_abs);
    }
    const int seg = clamped_idx - foreground_layer_count;
    const double low_abs = background_depth_edges[static_cast<std::size_t>(seg)];
    const double high_abs = background_depth_edges[static_cast<std::size_t>(seg + 1)];
    return 0.5 * (low_abs + high_abs);
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

struct MaterialRegistryKey {
    SDL_Texture* texture = nullptr;
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;

    bool operator==(const MaterialRegistryKey& other) const {
        return texture == other.texture && blend_mode == other.blend_mode;
    }
};

struct MaterialRegistryKeyHash {
    std::size_t operator()(const MaterialRegistryKey& key) const {
        const std::size_t texture_hash = std::hash<SDL_Texture*>{}(key.texture);
        const std::size_t blend_hash = std::hash<int>{}(static_cast<int>(key.blend_mode));
        return texture_hash ^ (blend_hash + 0x9e3779b9u + (texture_hash << 6) + (texture_hash >> 2));
    }
};

std::uint32_t resolve_material_index(
    const MaterialRegistryKey& key,
    std::unordered_map<MaterialRegistryKey, std::uint32_t, MaterialRegistryKeyHash>& registry,
    render_pipeline::LayerBuildResult& result) {
    const auto it = registry.find(key);
    if (it != registry.end()) {
        return it->second;
    }

    const std::uint32_t next_index = static_cast<std::uint32_t>(result.materials.size());
    result.materials.push_back(render_pipeline::DrawMaterial{key.texture, key.blend_mode});
    registry.emplace(key, next_index);
    return next_index;
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
        render_depth::build_linear_depth_edges(safe_max_cull_depth, base_layer_interval);
    const std::vector<double> background_depth_edges =
        render_depth::build_background_depth_edges(safe_max_cull_depth, base_layer_interval, depth_curve);

    const int foreground_layer_count = std::max(1, static_cast<int>(foreground_depth_edges.size()) - 1);
    const int background_layer_count = std::max(1, static_cast<int>(background_depth_edges.size()) - 1);
    const int layer_count = foreground_layer_count + background_layer_count;
    const std::size_t estimated_draw_count = geometry_batcher.item_count();

    result.layer_count = layer_count;
    result.layers.resize(static_cast<std::size_t>(layer_count));
    result.packed_vertices.reserve(estimated_draw_count * 4);
    result.packed_indices.reserve(estimated_draw_count * 6);
    result.packets.reserve(estimated_draw_count);
    result.gpu_packets.reserve(estimated_draw_count);
    result.materials.reserve(std::min<std::size_t>(estimated_draw_count, 256));
    std::unordered_map<MaterialRegistryKey, std::uint32_t, MaterialRegistryKeyHash> material_registry;

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
        if (layer.packet_indices.empty()) {
            const double midpoint = layer_midpoint_depth(layer_idx,
                                                         foreground_layer_count,
                                                         layer_count,
                                                         foreground_depth_edges,
                                                         background_depth_edges);
            layer.representative_depth = midpoint;
            layer.depth_min = std::isfinite(item.depth) ? item.depth : midpoint;
            layer.depth_max = std::isfinite(item.depth) ? item.depth : midpoint;
            layer.bounds_min_x = std::numeric_limits<float>::infinity();
            layer.bounds_min_y = std::numeric_limits<float>::infinity();
            layer.bounds_max_x = -std::numeric_limits<float>::infinity();
            layer.bounds_max_y = -std::numeric_limits<float>::infinity();
        }

        const MaterialRegistryKey material_key{item.texture, item.blend_mode};
        const std::uint32_t material_index = resolve_material_index(material_key, material_registry, result);
        const std::uint32_t vertex_offset = static_cast<std::uint32_t>(result.packed_vertices.size());
        result.packed_vertices.push_back(item.vertices[0]);
        result.packed_vertices.push_back(item.vertices[1]);
        result.packed_vertices.push_back(item.vertices[2]);
        result.packed_vertices.push_back(item.vertices[3]);

        const std::uint32_t index_offset = static_cast<std::uint32_t>(result.packed_indices.size());
        result.packed_indices.push_back(static_cast<int>(vertex_offset + 0));
        result.packed_indices.push_back(static_cast<int>(vertex_offset + 1));
        result.packed_indices.push_back(static_cast<int>(vertex_offset + 2));
        result.packed_indices.push_back(static_cast<int>(vertex_offset + 0));
        result.packed_indices.push_back(static_cast<int>(vertex_offset + 2));
        result.packed_indices.push_back(static_cast<int>(vertex_offset + 3));

        render_pipeline::DrawPacket packet{};
        const float packet_depth = std::isfinite(item.depth)
            ? static_cast<float>(item.depth)
            : static_cast<float>(layer.representative_depth);
        packet.vertex_offset = vertex_offset;
        packet.vertex_count = 4;
        packet.index_offset = index_offset;
        packet.index_count = 6;
        packet.material_index = material_index;
        packet.layer_index = static_cast<std::uint32_t>(layer_idx);
        packet.light_cluster_index = static_cast<std::uint32_t>(layer_idx);
        packet.depth = packet_depth;
        const std::uint32_t packet_index = static_cast<std::uint32_t>(result.packets.size());
        result.packets.push_back(packet);
        result.gpu_packets.push_back(render_pipeline::GpuDrawPacketRecord{
            packet.index_offset,
            packet.index_count,
            packet.vertex_offset,
            packet.vertex_count,
            packet.material_index,
            packet.layer_index,
            packet.light_cluster_index,
            packet.depth});

        layer.packet_indices.push_back(packet_index);
        if (layer.command_ranges.empty() ||
            layer.command_ranges.back().material_index != material_index ||
            layer.command_ranges.back().index_offset + layer.command_ranges.back().index_count != index_offset) {
            layer.command_ranges.push_back(render_pipeline::DrawCommandRange{
                material_index,
                index_offset,
                6,
                packet_index,
                1});
        } else {
            render_pipeline::DrawCommandRange& range = layer.command_ranges.back();
            range.index_count += 6;
            range.packet_count += 1;
        }

        if (std::isfinite(item.depth)) {
            layer.depth_min = std::min(layer.depth_min, item.depth);
            layer.depth_max = std::max(layer.depth_max, item.depth);
        }
        expand_layer_bounds(layer, std::array<SDL_Vertex, 4>{
            item.vertices[0], item.vertices[1], item.vertices[2], item.vertices[3]});
    });

    for (int i = 0; i < layer_count; ++i) {
        auto& layer = result.layers[static_cast<std::size_t>(i)];
        if (layer.packet_indices.empty() && layer.draws.empty()) {
            continue;
        }
        if (!std::isfinite(layer.depth_min) || !std::isfinite(layer.depth_max)) {
            layer.depth_min = layer.representative_depth;
            layer.depth_max = layer.representative_depth;
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
