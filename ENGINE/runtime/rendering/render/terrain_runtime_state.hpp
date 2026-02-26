#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "rendering/render/terrain_settings.hpp"

// Immutable snapshot of terrain config + derived runtime parameters for a single map session.
struct TerrainRuntimeState {
    TerrainSettings settings{};                 // Sanitized copy of the config used for sampling/shading.
    std::uint64_t   session_seed = 0;           // Finalized seed (base_seed + map_id [+ randomness]).
    SDL_FPoint      light_direction_world{-0.45f, -0.88f}; // Normalized XY light direction (Z assumed up).
    SDL_FPoint      light_anchor_world{0.0f, -800.0f};     // Frozen positional anchor for attenuation.
    std::uint64_t   revision = 0;               // Monotonic revision used to invalidate caches.

    // Build a runtime state from sanitized settings and map metadata.
    static TerrainRuntimeState from_settings(const TerrainSettings& settings,
                                             const std::string& map_id,
                                             bool randomize_session_seed);
};

