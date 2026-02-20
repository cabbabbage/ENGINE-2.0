#pragma once

#include <SDL3/SDL.h>
#include <cmath>
#include <limits>
#include <string>

namespace world { struct GridPoint; }

struct DisplacedAssetAnchorPoint {
    std::string name;
    int         texture_x = 0;   // Pixel coordinate on the sprite texture (X axis)
    int         texture_z = 0;   // Pixel coordinate on the sprite texture (vertical axis)
    bool        in_front = true; // True = one pixel closer to camera than owner, false = one pixel behind
    bool        has_pixel_coords = false;
    float       normalized_x = std::numeric_limits<float>::quiet_NaN();
    float       normalized_z = std::numeric_limits<float>::quiet_NaN();
    bool        has_normalized_coords = false;

    DisplacedAssetAnchorPoint() = default;
    DisplacedAssetAnchorPoint(std::string name_,
                              int tex_x,
                              int tex_z,
                              bool front = true)
        : name(std::move(name_))
        , texture_x(tex_x)
        , texture_z(tex_z)
        , in_front(front)
        , has_pixel_coords(true) {}

    bool is_valid() const {
        return !name.empty() && (has_pixel_coords || has_normalized_coords);
    }
};

struct ResolvedAnchor {
    SDL_Point        world_px{0, 0};
    int              world_z = 0;
    int              resolution_layer = 0;
    world::GridPoint* grid_point = nullptr;
    bool             missing = false;
    bool             in_front = true;
};

namespace anchor_points {

enum class GridMaterialization {
    None,
    Ensure
};

}
