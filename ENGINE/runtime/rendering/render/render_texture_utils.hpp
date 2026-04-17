#pragma once

#include <SDL3/SDL.h>

namespace render_texture_utils {

inline void reset_texture_state(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(texture, 255);
    SDL_SetTextureColorMod(texture, 255, 255, 255);
}

inline void draw_fullscreen_texture(SDL_Renderer* renderer, SDL_Texture* texture) {
    if (!renderer || !texture) {
        return;
    }
    reset_texture_state(texture);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
}

} // namespace render_texture_utils
