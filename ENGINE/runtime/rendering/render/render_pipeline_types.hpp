#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "rendering/render/layer_effect_processor.hpp"

namespace render_pipeline {

struct GeometryLayerDrawItem {
    SDL_Texture* texture = nullptr;
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    std::array<SDL_Vertex, 4> vertices{};
    double depth = 0.0;
};

struct DrawMaterial {
    SDL_Texture* texture = nullptr;
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
};

struct DrawPacket {
    std::uint32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material_index = 0;
    std::uint32_t layer_index = 0;
    std::uint32_t light_cluster_index = 0;
    float depth = 0.0f;
};

struct DrawCommandRange {
    std::uint32_t material_index = 0;
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    std::uint32_t packet_offset = 0;
    std::uint32_t packet_count = 0;
};

struct LayerSubmission {
    // Compatibility fallback for legacy tests/manual construction.
    std::vector<GeometryLayerDrawItem> draws;
    std::vector<std::uint32_t> packet_indices;
    std::vector<DrawCommandRange> command_ranges;
    double representative_depth = 0.0;
    double depth_min = 0.0;
    double depth_max = 0.0;
    float bounds_min_x = 0.0f;
    float bounds_min_y = 0.0f;
    float bounds_max_x = 0.0f;
    float bounds_max_y = 0.0f;
};

struct GpuMaterialRecord {
    std::uint64_t texture_token = 0;
    std::uint32_t blend_mode = static_cast<std::uint32_t>(SDL_BLENDMODE_BLEND);
    std::uint32_t reserved = 0;
};

struct GpuRuntimeLightRecord {
    float screen_center_x = 0.0f;
    float screen_center_y = 0.0f;
    float radius_px = 0.0f;
    float radius_world = 0.0f;
    float world_z = 0.0f;
    float intensity = 0.0f;
    float falloff = 0.0f;
    std::uint32_t color_rgba = 0;
};

struct GpuDrawPacketRecord {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t first_vertex = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t material_index = 0;
    std::uint32_t layer_index = 0;
    std::uint32_t light_cluster_index = 0;
    float depth = 0.0f;
};

struct LayerBuildResult {
    bool valid = false;
    int layer_count = 0;
    int player_layer_index = 0;
    std::vector<int> non_empty_layers;
    std::vector<LayerSubmission> layers;
    std::vector<SDL_Vertex> packed_vertices;
    std::vector<int> packed_indices;
    std::vector<DrawPacket> packets;
    std::vector<DrawMaterial> materials;
    std::vector<GpuDrawPacketRecord> gpu_packets;
};

struct GpuSubmissionStats {
    bool active = false;
    std::size_t vertex_capacity_bytes = 0;
    std::size_t index_capacity_bytes = 0;
    std::size_t material_capacity_bytes = 0;
    std::size_t light_capacity_bytes = 0;
    std::size_t packet_capacity_bytes = 0;
    std::uint64_t buffer_create_count = 0;
    std::uint64_t buffer_destroy_count = 0;
};

struct GpuCompactRenderStats {
    std::uint32_t tile_count_x = 0;
    std::uint32_t tile_count_y = 0;
    std::uint32_t tile_size_px = 0;
    std::uint32_t tile_light_assignment_count = 0;
    std::uint32_t naive_light_evaluations = 0;
    std::uint32_t tiled_light_evaluations = 0;
    std::uint32_t aggregated_light_count = 0;
};

struct LayerRenderResult {
    bool valid = false;
    int layer_count = 0;
    int player_layer_index = 0;
    std::vector<int> non_empty_layers;
    std::vector<SDL_Texture*> final_layer_textures;
    std::vector<std::vector<LayerEffectProcessor::RuntimeLight>> owning_body_lights;
    GpuSubmissionStats gpu_submission{};
};

struct CompactLayerRenderResult {
    bool valid = false;
    SDL_Texture* final_texture = nullptr;
    GpuSubmissionStats gpu_submission{};
    GpuCompactRenderStats compact_stats{};
};

struct BlurCompositeResult {
    bool valid = false;
    SDL_Texture* background_mid = nullptr;
    SDL_Texture* foreground_mid = nullptr;
};

} // namespace render_pipeline
