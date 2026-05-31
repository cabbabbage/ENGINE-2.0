#pragma once

#include <SDL3/SDL.h>

namespace render_texture_utils {

inline SDL_BlendMode premultiplied_over_blend_mode() {
    static const SDL_BlendMode mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD);
    return mode;
}

inline void reset_texture_state(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(texture, 255);
    SDL_SetTextureColorMod(texture, 255, 255, 255);
}

inline void reset_premultiplied_texture_state(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, premultiplied_over_blend_mode());
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

inline void draw_fullscreen_premultiplied_texture(SDL_Renderer* renderer, SDL_Texture* texture) {
    if (!renderer || !texture) {
        return;
    }
    reset_premultiplied_texture_state(texture);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
}

} // namespace render_texture_utils
