#pragma once

#include <SDL3/SDL.h>
#include "utils/sdl_mouse_utils.hpp"
#include <cmath>

inline SDL_Point event_point_from_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        return SDL_Point{
            static_cast<int>(std::lround(e.motion.x)),
            static_cast<int>(std::lround(e.motion.y))};
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        return SDL_Point{
            static_cast<int>(std::lround(e.button.x)),
            static_cast<int>(std::lround(e.button.y))};
    }
    int mx = 0;
    int my = 0;
    sdl_mouse_util::GetMouseState(&mx, &my);
    return SDL_Point{mx, my};
}

