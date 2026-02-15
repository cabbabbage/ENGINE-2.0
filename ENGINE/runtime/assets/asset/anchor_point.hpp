#pragma once

#include <SDL3/SDL.h>
#include <cmath>
#include <string>

namespace world { struct GridPoint; }

struct DisplacedAssetAnchorPoint {
    std::string name;
    float       px = 0.0f;
    float       py = 0.0f;
    float       pz = 0.0f;
    float       rotation_deg = 0.0f;

    bool is_valid() const {
        return !name.empty() &&
               std::isfinite(px) &&
               std::isfinite(py) &&
               std::isfinite(pz) &&
               std::isfinite(rotation_deg);
    }
};

struct ResolvedAnchor {
    SDL_Point        world_px{0, 0};
    int              world_z = 0;
    int              resolution_layer = 0;
    world::GridPoint* grid_point = nullptr;
    float            rotation_deg = 0.0f;
    bool             missing = false;
};

namespace anchor_points {

enum class GridMaterialization {
    None,
    Ensure
};

}
