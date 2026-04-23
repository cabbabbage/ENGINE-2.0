#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

namespace render_pipeline {

struct RuntimeLight {
    std::uint64_t stable_light_id = 0;
    SDL_FPoint screen_center{0.0f, 0.0f};
    SDL_Color color{255, 255, 255, 255};
    float intensity = 0.0f;
    float opacity = 0.0f;
    float radius_px = 0.0f;
    float radius_world = 0.0f;
    float falloff = 1.0f;
    float world_z = 0.0f;
    float floor_world_x = 0.0f;
    float floor_world_z = 0.0f;
    float world_height = 0.0f;
    SDL_FPoint floor_screen_center{0.0f, 0.0f};
    bool has_floor_projection = false;
    float signed_depth_to_focus = 0.0f;
    float depth_overlap_weight = 1.0f;
    SDL_FPoint dominant_screen_direction{0.0f, -1.0f};
};

struct GeometryLayerDrawItem {
    SDL_Texture* texture = nullptr;
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    std::array<SDL_Vertex, 4> vertices{};
    double depth = 0.0;
};

struct LayerSubmission {
    std::vector<GeometryLayerDrawItem> draws;
    double representative_depth = 0.0;
    double depth_min = 0.0;
    double depth_max = 0.0;
    float bounds_min_x = 0.0f;
    float bounds_min_y = 0.0f;
    float bounds_max_x = 0.0f;
    float bounds_max_y = 0.0f;
};

struct LayerBuildResult {
    bool valid = false;
    int layer_count = 0;
    int player_layer_index = 0;
    std::vector<int> non_empty_layers;
    std::vector<LayerSubmission> layers;
};

struct LayerRenderResult {
    bool valid = false;
    int layer_count = 0;
    int player_layer_index = 0;
    std::vector<int> non_empty_layers;
    std::vector<SDL_Texture*> final_layer_textures;
};

struct BlurCompositeResult {
    bool valid = false;
    SDL_Texture* background_mid = nullptr;
    SDL_Texture* foreground_mid = nullptr;
};

} // namespace render_pipeline
