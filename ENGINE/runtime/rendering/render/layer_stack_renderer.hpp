#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <vector>

#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/render_pipeline_types.hpp"

class LayerStackRenderer {
public:
    explicit LayerStackRenderer(SDL_Renderer* renderer);
    ~LayerStackRenderer();

    LayerStackRenderer(const LayerStackRenderer&) = delete;
    LayerStackRenderer& operator=(const LayerStackRenderer&) = delete;

    void set_output_dimensions(int screen_width, int screen_height);

    render_pipeline::LayerRenderResult render(
        const render_pipeline::LayerBuildResult& build,
        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
        bool runtime_lighting_enabled,
        float front_layer_light_strength_multiplier,
        float behind_layer_light_strength_multiplier);

private:
    struct FrameScratchArena {
        std::vector<std::vector<std::uint32_t>> per_layer_light_indices;
        std::vector<LayerEffectProcessor::RuntimeLight> layer_light_buffer;
        std::vector<LayerEffectProcessor::RuntimeLight> owner_light_buffer;
        std::vector<double> layer_depth_min;
        std::vector<double> layer_depth_max;
        std::vector<float> layer_bounds_min_x;
        std::vector<float> layer_bounds_min_y;
        std::vector<float> layer_bounds_max_x;
        std::vector<float> layer_bounds_max_y;

        void clear_for_frame(std::size_t layer_count) {
            if (per_layer_light_indices.size() < layer_count) {
                per_layer_light_indices.resize(layer_count);
            }
            for (std::size_t i = 0; i < layer_count; ++i) {
                per_layer_light_indices[i].clear();
            }
            layer_light_buffer.clear();
            owner_light_buffer.clear();
            layer_depth_min.resize(layer_count);
            layer_depth_max.resize(layer_count);
            layer_bounds_min_x.resize(layer_count);
            layer_bounds_min_y.resize(layer_count);
            layer_bounds_max_x.resize(layer_count);
            layer_bounds_max_y.resize(layer_count);
        }
    };

    struct TextureSet {
        SDL_Texture* base = nullptr;
        SDL_Texture* dark_mask = nullptr;
        SDL_Texture* dark_mask_merged = nullptr;
        SDL_Texture* lit = nullptr;
        std::uint8_t dark_mask_history_write_index = 0;
        std::uint8_t valid_dark_mask_history_count = 0;
    };

    bool ensure_target(SDL_Texture*& texture) const;
    bool ensure_layer_capacity(int layer_count);
    void reset_targets();
    void clear_target(SDL_Texture* texture) const;
    bool copy_texture(SDL_Texture* src, SDL_Texture* dst) const;
  

    void render_layer_base(const render_pipeline::LayerSubmission& layer,
                           SDL_Texture* target) const;

    SDL_Renderer* renderer_ = nullptr;
    int screen_width_ = 0;
    int screen_height_ = 0;
    LayerEffectProcessor layer_effect_processor_;
    std::vector<TextureSet> layer_targets_;
    FrameScratchArena frame_scratch_;
};
