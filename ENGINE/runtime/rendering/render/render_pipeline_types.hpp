#pragma once

#include <array>
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
    std::vector<std::vector<LayerEffectProcessor::RuntimeLight>> owning_body_lights;
};

struct BlurCompositeResult {
    bool valid = false;
    SDL_Texture* background_mid = nullptr;
    SDL_Texture* foreground_mid = nullptr;
};

} // namespace render_pipeline
