#pragma once

#include <SDL.h>

#include <algorithm>
#include <cmath>

#include "utils/grid.hpp"

namespace devmode::frame_editors {

inline SDL_FPoint snap_world_point_to_grid(const SDL_FPoint& world, int resolution) {
    const int snapped_resolution = vibble::grid::clamp_resolution(std::max(0, resolution));
    SDL_Point world_px{
        static_cast<int>(std::lround(world.x)),
        static_cast<int>(std::lround(world.y))
    };
    SDL_Point snapped = vibble::grid::snap_world_to_vertex(world_px, snapped_resolution);
    return SDL_FPoint{static_cast<float>(snapped.x), static_cast<float>(snapped.y)};
}

inline float snap_world_z_to_grid(float world_z, int resolution) {
    const int snapped_resolution = vibble::grid::clamp_resolution(std::max(0, resolution));
    const int step = vibble::grid::delta(snapped_resolution);
    if (step <= 0) {
        return world_z;
    }
    const double ratio = static_cast<double>(world_z) / static_cast<double>(step);
    const double rounded = std::llround(ratio);
    return static_cast<float>(rounded * static_cast<double>(step));
}

}  // namespace devmode::frame_editors
