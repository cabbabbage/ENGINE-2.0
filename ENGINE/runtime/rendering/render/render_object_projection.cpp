#include "rendering/render/render_object_projection.hpp"

#include <algorithm>
#include <cmath>

#include "rendering/render/warped_screen_grid.hpp"

namespace render_projection {
namespace {
bool is_finite_screen_point(const SDL_FPoint& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool resolve_frame_dimensions(const RenderObject& obj, int& frame_w, int& frame_h) {
    frame_w = obj.texture_w;
    frame_h = obj.texture_h;
    if (obj.has_src_rect && obj.src_rect.w > 0 && obj.src_rect.h > 0) {
        frame_w = obj.src_rect.w;
        frame_h = obj.src_rect.h;
    }

    if (frame_w > 0 && frame_h > 0) {
        return true;
    }

    if (!obj.texture) {
        return false;
    }

    float tex_w = 0.0f;
    float tex_h = 0.0f;
    if (!SDL_GetTextureSize(obj.texture, &tex_w, &tex_h)) {
        return false;
    }

    frame_w = std::max(1, static_cast<int>(std::lround(tex_w)));
    frame_h = std::max(1, static_cast<int>(std::lround(tex_h)));
    return true;
}

SDL_FPoint sanitize_anchor_uv(SDL_FPoint uv) {
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        return SDL_FPoint{0.5f, 1.0f};
    }
    return SDL_FPoint{
        std::clamp(uv.x, 0.0f, 1.0f),
        std::clamp(uv.y, 0.0f, 1.0f)};
}
} // namespace

float sanitize_perspective_scale(float perspective_scale) {
    return (std::isfinite(perspective_scale) && perspective_scale > 0.0f) ? perspective_scale : 1.0f;
}

bool assemble_render_object_projection_input(const RenderObject& obj,
                                             float perspective_scale,
                                             float world_z,
                                             SpriteProjectionInput& out_input) {
    out_input = SpriteProjectionInput{};
    if (!obj.texture || obj.screen_rect.w <= 0 || obj.screen_rect.h <= 0) {
        return false;
    }

    const float final_width_px = static_cast<float>(obj.screen_rect.w);
    const float final_height_px = static_cast<float>(obj.screen_rect.h);
    if (!(std::isfinite(final_width_px) && std::isfinite(final_height_px) &&
          final_width_px > 0.0f && final_height_px > 0.0f)) {
        return false;
    }

    int frame_w = 0;
    int frame_h = 0;
    if (!resolve_frame_dimensions(obj, frame_w, frame_h)) {
        return false;
    }

    out_input.world_x = static_cast<float>(obj.screen_rect.x);
    out_input.world_y = static_cast<float>(obj.screen_rect.y);
    out_input.world_z = world_z;
    out_input.perspective_scale = sanitize_perspective_scale(perspective_scale);
    out_input.frame_width_px = std::max(1, frame_w);
    out_input.frame_height_px = std::max(1, frame_h);
    out_input.final_width_px = std::max(1, static_cast<int>(std::lround(final_width_px)));
    out_input.final_height_px = std::max(1, static_cast<int>(std::lround(final_height_px)));
    out_input.flip = obj.flip;
    out_input.anchor_uv = sanitize_anchor_uv(obj.projection_anchor_uv);
    return true;
}

bool build_render_object_projected_frame(const WarpedScreenGrid& cam,
                                         const RenderObject& obj,
                                         float perspective_scale,
                                         float world_z,
                                         ProjectedSpriteFrame& out_projection) {
    out_projection = ProjectedSpriteFrame{};

    SpriteProjectionInput projection_input{};
    if (!assemble_render_object_projection_input(obj, perspective_scale, world_z, projection_input)) {
        return false;
    }

    if (!build_projected_sprite_frame(cam, projection_input, out_projection) || !out_projection.valid) {
        return false;
    }

    return is_finite_screen_point(out_projection.screen_tl) &&
           is_finite_screen_point(out_projection.screen_tr) &&
           is_finite_screen_point(out_projection.screen_br) &&
           is_finite_screen_point(out_projection.screen_bl);
}

} // namespace render_projection
