#pragma once

#include <SDL3/SDL.h>
#include <algorithm>
#include <string>

namespace world { struct GridPoint; }

// Canonical anchor contract:
// - texture_x/texture_y name an integer pixel in the un-flipped source texture.
// - The anchor refers to the *center* of that pixel: (x + 0.5, y + 0.5).
// - +X goes right, +Y goes down in texture space; Z is derived at runtime.
// - Horizontal flips are applied after converting to UV: u = 1 - u when flipped.
// - The same contract is consumed by runtime projection and the editor preview.
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

// Convert a canonical anchor pixel to normalized UV, applying optional horizontal flip.
inline SDL_FPoint anchor_pixel_to_uv(SDL_Point texture_px,
                                     int texture_w,
                                     int texture_h,
                                     SDL_FlipMode flip = SDL_FLIP_NONE) {
    auto to_unit = [](int pixel, int dimension) {
        if (dimension <= 0) {
            return 0.5f;
        }
        return std::clamp((static_cast<float>(pixel) + 0.5f) / static_cast<float>(dimension), 0.0f, 1.0f);
    };
    const float frame_u = to_unit(texture_px.x, texture_w);
    const float frame_v = to_unit(texture_px.y, texture_h);
    const bool flip_h = (flip & SDL_FLIP_HORIZONTAL) != 0;
    return SDL_FPoint{flip_h ? (1.0f - frame_u) : frame_u, frame_v};
}

}
