#pragma once

#include <SDL.h>
#include <algorithm>
#include <cmath>

namespace vibble {

struct PointPercentage {
    float px = 0.0f; // Percentage of asset height for X axis
    float py = 0.0f; // Percentage of asset height for Y axis
    float pz = 0.0f; // Percentage of asset height for Z axis

    static PointPercentage from_world(const SDL_FPoint& world_offset, float world_z, float asset_height) {
        PointPercentage pp;
        if (asset_height <= 0.0f) return pp;
        
        pp.px = world_offset.x / asset_height;
        pp.py = world_offset.y / asset_height;
        pp.pz = world_z / asset_height;
        return pp;
    }

    struct WorldPoint {
        SDL_FPoint xy;
        float z;
    };

    WorldPoint to_world(float asset_height) const {
        WorldPoint wp;
        wp.xy.x = px * asset_height;
        wp.xy.y = py * asset_height;
        wp.z = pz * asset_height;
        return wp;
    }
};

} // namespace vibble
