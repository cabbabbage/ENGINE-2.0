#include "rendering/render/scene_composite_pass.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/render_texture_utils.hpp"

SceneCompositePass::SceneCompositePass(SDL_Renderer* renderer)
    : renderer_(renderer) {}

bool SceneCompositePass::compose(SDL_Texture* gameplay_target,
                                 const render_pipeline::LayerRenderResult& layer_render,
                                 const render_pipeline::BlurCompositeResult& blur_result) {
    if (!renderer_ || !gameplay_target) {
        return false;
    }

    if (!render_diagnostics::set_render_target(renderer_, gameplay_target)) {
        return false;
    }
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (blur_result.valid) {
        render_texture_utils::draw_fullscreen_texture(renderer_, blur_result.background_mid);
        render_texture_utils::draw_fullscreen_texture(renderer_, blur_result.foreground_mid);
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
        render_texture_utils::draw_fullscreen_texture(renderer_, layer_render.final_layer_textures[static_cast<std::size_t>(layer_index)]);
    }

    return !layer_render.non_empty_layers.empty();
}

bool SceneCompositePass::compose_gpu(SDL_Texture* gameplay_target,
                                     SDL_Texture* floor_texture,
                                     SDL_Texture* floor_dark_mask_texture,
                                     SDL_Texture* scene_texture,
                                     const render_pipeline::BlurCompositeResult& blur_result) {
    if (!renderer_ || !gameplay_target) {
        return false;
    }

    if (!render_diagnostics::set_render_target(renderer_, gameplay_target)) {
        return false;
    }
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);

    if (floor_texture) {
        render_texture_utils::draw_fullscreen_texture(renderer_, floor_texture);
    }

    if (floor_dark_mask_texture) {
        SDL_SetTextureBlendMode(floor_dark_mask_texture, SDL_BLENDMODE_MOD);
        SDL_SetTextureAlphaMod(floor_dark_mask_texture, 255);
        SDL_SetTextureColorMod(floor_dark_mask_texture, 255, 255, 255);
        render_diagnostics::render_texture(renderer_, floor_dark_mask_texture, nullptr, nullptr);
    }

    SDL_Texture* resolved_scene = scene_texture;
    if (blur_result.valid && blur_result.background_mid) {
        resolved_scene = blur_result.background_mid;
    }
    render_texture_utils::draw_fullscreen_texture(renderer_, resolved_scene);

    if (blur_result.valid && blur_result.foreground_mid) {
        render_texture_utils::draw_fullscreen_texture(renderer_, blur_result.foreground_mid);
    }

    return floor_texture || floor_dark_mask_texture || resolved_scene != nullptr;
}
