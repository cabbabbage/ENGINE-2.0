#pragma once

#include "sdl3_render_compat.hpp"

namespace devmode::sdl {

bool is_pointer_event(const SDL_Event& e);

SDL_Point event_point(const SDL_Event& e);

}

