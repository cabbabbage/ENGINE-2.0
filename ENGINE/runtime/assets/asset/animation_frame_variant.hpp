#pragma once

#include <SDL3/SDL.h>

class FrameVariant {
public:
    int varient = -1;
    SDL_Texture* base_texture = nullptr;
    SDL_Rect source_rect{0, 0, 0, 0};
    bool uses_atlas = false;
    SDL_Texture* get_base_texture() const { return base_texture; }
};
