#pragma once

#include <SDL3/SDL.h>

namespace render_floor_clip {

enum class Visibility {
    FullyVisible,
    PartiallyVisible,
    FullyBuried
};

struct ClipInput {
    float anchor_world_y = 0.0f;
    float anchor_v = 1.0f;
    float world_height = 0.0f;
};

struct ClipResult {
    bool valid = false;
    Visibility visibility = Visibility::FullyVisible;
    float visible_v = 1.0f;
};

struct AtlasUvRect {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

ClipResult compute_floor_clip(const ClipInput& input);

SDL_FPoint map_local_uv_to_atlas(SDL_FPoint local_uv,
                                 const AtlasUvRect& atlas,
                                 bool flip_horizontal,
                                 bool flip_vertical);

} // namespace render_floor_clip
