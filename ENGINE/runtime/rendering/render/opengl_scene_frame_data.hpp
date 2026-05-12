#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

struct GpuSpriteVertex {
    float clip_x = 0.0f;
    float clip_y = 0.0f;
    float uv_x = 0.0f;
    float uv_y = 0.0f;
};

struct GpuSpriteDrawPacket {
    SDL_Texture* source_texture = nullptr;
    SDL_GPUTexture* source_gpu_texture = nullptr;
    std::string source_asset_name{};
    std::string source_animation_name{};
    std::string source_texture_id{};
    int source_frame_index = -1;
    int source_variant_index = -1;
    std::array<GpuSpriteVertex, 6> vertices{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint8_t sort_group = 0;
    float sort_key = 0.0f;
    float depth_metric = 0.0f;
    std::uintptr_t stable_sort_id = 0u;
    bool is_floor_packet = false;
    int depth_layer = 0;
};

struct GpuDepthLayerDrawPackets {
    int depth_layer = 0;
    float blur_strength_px = 0.0f;
    std::vector<GpuSpriteDrawPacket> packets{};
};

struct GpuSceneFrameData {
    std::vector<GpuSpriteDrawPacket> floor_draws{};
    std::vector<GpuSpriteDrawPacket> xy_sprite_draws{};
    std::vector<GpuDepthLayerDrawPackets> depth_layers{};
    std::uint32_t target_width = 0;
    std::uint32_t target_height = 0;
    std::uint32_t floor_draw_count = 0;
    std::uint32_t xy_sprite_draw_count = 0;
    std::uint32_t active_depth_layer_count = 0;
    std::uint32_t debug_overlay_draw_count = 0;
    std::uint32_t active_asset_count = 0;
    std::uint32_t filtered_active_asset_count = 0;
    std::uint32_t selected_asset_count = 0;
    std::uint32_t visible_traversal_count = 0;
    SDL_Texture* ui_overlay_texture = nullptr;
    SDL_GPUTexture* ui_overlay_gpu_texture = nullptr;
    bool dev_mode = false;
    bool focus_filter_active = false;
    bool used_active_asset_fallback = false;
    bool suspected_incomplete_scene = false;
    bool has_valid_composite_source = false;
};
