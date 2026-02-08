#pragma once

#include <SDL3/SDL.h>

#include <cmath>

namespace sdl_mouse_util {

inline SDL_Point GetMousePoint() {
    float fx = 0.0f;
    float fy = 0.0f;
    SDL_GetMouseState(&fx, &fy);
    return SDL_Point{static_cast<int>(std::lround(fx)), static_cast<int>(std::lround(fy))};
}

inline SDL_MouseButtonFlags GetMouseState(int* x, int* y) {
    float fx = 0.0f;
    float fy = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(x ? &fx : nullptr, y ? &fy : nullptr);
    if (x) {
        *x = static_cast<int>(std::lround(fx));
    }
    if (y) {
        *y = static_cast<int>(std::lround(fy));
    }
    return buttons;
}

}  // namespace sdl_mouse_util
