#pragma once

#include <vector>

#include <SDL3/SDL.h>

#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/render_pipeline_types.hpp"

class LayerStackRenderer {
public:
    explicit LayerStackRenderer(SDL_Renderer* renderer);
    ~LayerStackRenderer();

    void set_output_dimensions(int screen_width, int screen_height);

    render_pipeline::LayerRenderResult render(
        const render_pipeline::LayerBuildResult& build,
        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
        bool runtime_lighting_enabled,
        float front_layer_light_strength_multiplier,
        float behind_layer_light_strength_multiplier);

private:
    struct TextureSet {
        SDL_Texture* base = nullptr;
        SDL_Texture* dark_mask = nullptr;
        SDL_Texture* lit = nullptr;
    };

    void reset_targets();
    bool ensure_target(SDL_Texture*& texture) const;
    bool ensure_layer_capacity(int layer_count);
    void clear_target(SDL_Texture* texture) const;
    void render_layer_base(const render_pipeline::LayerSubmission& layer, SDL_Texture* target) const;

    std::vector<LayerEffectProcessor::RuntimeLight> bias_lights_for_layer(
        const std::vector<LayerEffectProcessor::RuntimeLight>& source_lights,
        double layer_depth_min,
        double layer_depth_max,
        float front_layer_light_strength_multiplier,
        float behind_layer_light_strength_multiplier) const;

    std::vector<LayerEffectProcessor::RuntimeLight> collect_layer_lights(
        const render_pipeline::LayerSubmission& layer,
        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights) const;

    std::vector<LayerEffectProcessor::RuntimeLight> collect_owner_lights(
        const render_pipeline::LayerSubmission& layer,
        const std::vector<LayerEffectProcessor::RuntimeLight>& biased_lights) const;

    SDL_Renderer* renderer_ = nullptr;
    int screen_width_ = 1;
    int screen_height_ = 1;
    LayerEffectProcessor layer_effect_processor_;
    std::vector<TextureSet> layer_targets_;
};
