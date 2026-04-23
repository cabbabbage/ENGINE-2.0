#pragma once

#include <SDL3/SDL.h>

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

    render_pipeline::LayerRenderResult render(const render_pipeline::LayerBuildResult& build);

private:

    struct TextureSet {
        SDL_Texture* base = nullptr;
        SDL_Texture* lit = nullptr;
    };

    bool ensure_target(SDL_Texture*& texture) const;
    bool ensure_layer_capacity(int layer_count);
    void reset_targets();
    void clear_target(SDL_Texture* texture) const;

    void render_layer_base(const render_pipeline::LayerSubmission& layer,
                           SDL_Texture* target) const;

    SDL_Renderer* renderer_ = nullptr;
    int screen_width_ = 0;
    int screen_height_ = 0;
    LayerEffectProcessor layer_effect_processor_;
    std::vector<TextureSet> layer_targets_;
};
