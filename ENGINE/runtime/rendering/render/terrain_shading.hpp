#pragma once

#include <SDL3/SDL.h>

#include "rendering/render/terrain_runtime_state.hpp"

// Compute a stable brightness factor for terrain vertices based on slope and light settings.
float terrain_brightness(const TerrainRuntimeState& state,
                         float slope_x,
                         float slope_y,
                         const SDL_FPoint& world_position);

