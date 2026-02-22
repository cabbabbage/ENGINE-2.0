#pragma once

#include <SDL3/SDL.h>
#include <string>

namespace world { struct GridPoint; }

struct DisplacedAssetAnchorPoint {
    std::string name;
    int         texture_x = 0;   // Pixel coordinate on the sprite texture (X axis)
    int         texture_y = 0;   // Pixel coordinate on the sprite texture (vertical axis)
    bool        in_front = true; // True = one pixel closer to camera than owner, false = one pixel behind

    DisplacedAssetAnchorPoint() = default;
    DisplacedAssetAnchorPoint(std::string name_,
                              int tex_x,
                              int tex_y,
                              bool front = true)
        : name(std::move(name_))
        , texture_x(tex_x)
        , texture_y(tex_y)
        , in_front(front) {}

    bool is_valid() const {
        return !name.empty();
    }
};

struct ResolvedAnchor {
    SDL_Point        world_px{0, 0};
    int              world_z = 0;
    int              resolution_layer = 0;
    SDL_Point        source_texture_px{0, 0};
    bool             has_canonical_texture_source = false;
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
