#pragma once

#include <SDL3/SDL.h>

#include <cmath>

namespace sdl_mouse_util {

inline SDL_Point RoundFloatPoint(float fx, float fy) {
    return SDL_Point{
        static_cast<int>(std::lround(fx)),
        static_cast<int>(std::lround(fy)),
    };
}

inline SDL_Point GetMousePoint() {
    float fx = 0.0f;
    float fy = 0.0f;
    SDL_GetMouseState(&fx, &fy);
    return RoundFloatPoint(fx, fy);
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

inline SDL_Point MotionPoint(const SDL_MouseMotionEvent& motion) {
    return RoundFloatPoint(motion.x, motion.y);
}

inline SDL_Point ButtonPoint(const SDL_MouseButtonEvent& button) {
    return RoundFloatPoint(static_cast<float>(button.x), static_cast<float>(button.y));
}

inline SDL_Point WheelPoint(const SDL_MouseWheelEvent& wheel) {
    return RoundFloatPoint(wheel.mouse_x, wheel.mouse_y);
}

}  // namespace sdl_mouse_util
