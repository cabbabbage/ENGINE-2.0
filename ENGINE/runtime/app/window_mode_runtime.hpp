#pragma once

#include <SDL3/SDL.h>

namespace app::window_mode_runtime {

struct WindowedPlacement {
    int x = SDL_WINDOWPOS_CENTERED;
    int y = SDL_WINDOWPOS_CENTERED;
    int w = 1280;
    int h = 720;
};

WindowedPlacement compute_windowed_fallback(SDL_Window* window);

bool toggle_fullscreen(SDL_Window* window,
                       bool& is_fullscreen,
                       int& windowed_x,
                       int& windowed_y,
                       int& windowed_width,
                       int& windowed_height);

} // namespace app::window_mode_runtime
