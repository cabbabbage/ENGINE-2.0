#include "rendering/render/scene_composite_pass.hpp"

namespace {

void reset_texture_state(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(texture, 255);
    SDL_SetTextureColorMod(texture, 255, 255, 255);
}

void draw_fullscreen(SDL_Renderer* renderer, SDL_Texture* texture) {
    if (!renderer || !texture) {
        return;
    }
    reset_texture_state(texture);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
}

} // namespace

SceneCompositePass::SceneCompositePass(SDL_Renderer* renderer)
    : renderer_(renderer) {}

bool SceneCompositePass::compose(SDL_Texture* gameplay_target,
                                 const render_pipeline::LayerRenderResult& layer_render,
                                 const render_pipeline::BlurCompositeResult& blur_result) {
    if (!renderer_ || !gameplay_target) {
        return false;
    }

    SDL_SetRenderTarget(renderer_, gameplay_target);
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (blur_result.valid) {
        draw_fullscreen(renderer_, blur_result.background_mid);
        draw_fullscreen(renderer_, blur_result.foreground_mid);
        return true;
    }

    if (!layer_render.valid) {
        return false;
    }

    for (auto it = layer_render.non_empty_layers.rbegin(); it != layer_render.non_empty_layers.rend(); ++it) {
        const int layer_index = *it;
        if (layer_index < 0 || static_cast<std::size_t>(layer_index) >= layer_render.final_layer_textures.size()) {
            continue;
        }
        draw_fullscreen(renderer_, layer_render.final_layer_textures[static_cast<std::size_t>(layer_index)]);
    }

    return !layer_render.non_empty_layers.empty();
}
