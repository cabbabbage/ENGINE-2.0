#pragma once

#include <SDL3/SDL.h>

#include "rendering/render/render_pipeline_types.hpp"

class SceneCompositePass {
public:
    explicit SceneCompositePass(SDL_Renderer* renderer);

    bool compose(SDL_Texture* gameplay_target,
                 const render_pipeline::LayerRenderResult& layer_render,
                 const render_pipeline::BlurCompositeResult& blur_result);
    bool compose_gpu(SDL_Texture* gameplay_target,
                     SDL_Texture* floor_texture,
                     SDL_Texture* floor_dark_mask_texture,
                     SDL_Texture* floor_overlay_texture,
                     SDL_Texture* scene_texture,
                     const render_pipeline::BlurCompositeResult& blur_result);

private:
    SDL_Renderer* renderer_ = nullptr;
};
