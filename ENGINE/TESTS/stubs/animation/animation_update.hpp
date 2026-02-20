#pragma once

#include <SDL3/SDL.h>

#include "gameplay/world/grid_point.hpp"

class Asset;

namespace animation_update::detail {

inline world::GridPoint bottom_middle_for(const Asset&, const world::GridPoint& pos) {
    return pos;
}

inline SDL_Point bottom_middle_for(const Asset&, SDL_Point pos) {
    return pos;
}

}  // namespace animation_update::detail

