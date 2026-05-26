#include "rendering/render/sprite_floor_clip.hpp"

#include <algorithm>
#include <cmath>

namespace render_floor_clip {
namespace {

float sanitize_unit(float value, float fallback) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

} // namespace

ClipResult compute_floor_clip(const ClipInput& input) {
    ClipResult result{};
    if (!std::isfinite(input.anchor_world_y) ||
        !std::isfinite(input.anchor_v) ||
        !std::isfinite(input.world_height) ||
        input.world_height <= 1.0e-5f) {
        return result;
    }

    const float anchor_v = sanitize_unit(input.anchor_v, 1.0f);
    const float floor_v = anchor_v + input.anchor_world_y / input.world_height;
    if (!std::isfinite(floor_v)) {
        return result;
    }

    result.valid = true;
    if (floor_v <= 0.0f) {
        result.visibility = Visibility::FullyBuried;
        result.visible_v = 0.0f;
        return result;
    }
    if (floor_v >= 1.0f) {
        result.visibility = Visibility::FullyVisible;
        result.visible_v = 1.0f;
        return result;
    }

    result.visibility = Visibility::PartiallyVisible;
    result.visible_v = floor_v;
    return result;
}

SDL_FPoint map_local_uv_to_atlas(SDL_FPoint local_uv,
                                 const AtlasUvRect& atlas,
                                 bool flip_horizontal,
                                 bool flip_vertical) {
    const float local_u = sanitize_unit(local_uv.x, 0.0f);
    const float local_v = sanitize_unit(local_uv.y, 0.0f);
    const float mapped_u = flip_horizontal ? (1.0f - local_u) : local_u;
    const float mapped_v = flip_vertical ? (1.0f - local_v) : local_v;
    return SDL_FPoint{
        lerp(atlas.u0, atlas.u1, mapped_u),
        lerp(atlas.v0, atlas.v1, mapped_v)
    };
}

} // namespace render_floor_clip
