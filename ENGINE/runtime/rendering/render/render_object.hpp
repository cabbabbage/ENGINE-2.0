#pragma once

#include <array>
#include <cstdint>
#include <limits>

#include <SDL3/SDL.h>

struct RenderObject {
    SDL_Texture* texture = nullptr;
    SDL_Rect screen_rect{};
    float world_anchor_x = std::numeric_limits<float>::quiet_NaN();
    float world_anchor_y = std::numeric_limits<float>::quiet_NaN();
    SDL_Color color_mod{255, 255, 255, 255};
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    double angle = 0.0;
    SDL_Point center{0, 0};
    bool use_custom_center = false;
    SDL_FlipMode flip = SDL_FLIP_NONE;
    int texture_w = 0;
    int texture_h = 0;
    bool has_texture_size = false;
    float world_z_offset = 0.0f;
    bool has_src_rect = false;
    SDL_Rect src_rect{0, 0, 0, 0};
    int atlas_w = 0;
    int atlas_h = 0;
    bool has_atlas_size = false;
    SDL_Texture* dimension_cache_texture = nullptr;
    std::array<SDL_Vertex, 4> cached_vertices{};
    std::array<int, 6> cached_indices{0, 1, 2, 0, 2, 3};
    SDL_FPoint cached_position{0.0f, 0.0f};
    float cached_world_z = 0.0f;
    float cached_scale = 0.0f;
    std::int64_t cached_position_key_x = 0;
    std::int64_t cached_position_key_y = 0;
    std::int64_t cached_world_z_key = 0;
    std::int64_t cached_scale_key = 0;
    bool cached_projection_key_valid = false;
    std::uint64_t cached_camera_state_version = 0;
    SDL_Texture* cached_mesh_texture = nullptr;
    bool has_cached_mesh = false;
    bool mesh_dirty = true;
    SDL_FPoint projection_anchor_uv{0.5f, 1.0f};
};
