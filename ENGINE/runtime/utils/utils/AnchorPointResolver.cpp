#include "AnchorPointResolver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "animation/animation_update.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/world_grid.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace {

#if defined(ENGINE_WORLD_TESTS)

}  // namespace

namespace anchor_points {

float anchor_height_px(const Asset& asset) {
    return asset.runtime_height_px();
}

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization) {
    PixelLockedAnchor result{};
    result.resolved.world_px = asset.world_point();
    result.resolved.world_z = asset.world_z();
    result.screen_px = SDL_FPoint{static_cast<float>(asset.world_x()), static_cast<float>(asset.world_y())};
    result.resolved.in_front = anchor.in_front;
    result.resolved.missing = false;
    return result;
}

ResolvedAnchor resolve_anchor_point(const Asset&, const DisplacedAssetAnchorPoint&, GridMaterialization) {
    return ResolvedAnchor{};
}

}

#else

struct FrameDimensions {
    int frame_w = 0;      // Unscaled frame width (texture space)
    int frame_h = 0;      // Unscaled frame height (texture space)
    int final_w = 0;      // Scaled dimensions used for world quad (pre-perspective)
    int final_h = 0;
    SDL_FlipMode flip = SDL_FLIP_NONE;
};

struct SpriteQuad {
    SDL_FPoint tl{};
    SDL_FPoint tr{};
    SDL_FPoint br{};
    SDL_FPoint bl{};
};

float safe_remainder_scale(const Asset& asset) {
    float remainder = asset.current_remaining_scale_adjustment;
    if (!std::isfinite(remainder) || remainder <= 0.0f) {
        remainder = 1.0f;
    }
    return remainder;
}

bool gather_frame_dimensions(const Asset& asset, FrameDimensions& out) {
    const AnimationFrame* frame = asset.current_frame;
    SDL_Texture* texture = nullptr;
    int frame_w = 0;
    int frame_h = 0;

    if (frame && !frame->variants.empty()) {
        const int variant_idx = std::clamp(asset.current_variant_index, 0, static_cast<int>(frame->variants.size()) - 1);
        const FrameVariant& variant = frame->variants[static_cast<std::size_t>(variant_idx)];
        texture = variant.get_base_texture();
        if (variant.source_rect.w > 0 && variant.source_rect.h > 0) {
            frame_w = variant.source_rect.w;
            frame_h = variant.source_rect.h;
        }
    }

    if (!texture) {
        texture = asset.get_current_frame();
    }

    if (frame_w <= 0 || frame_h <= 0) {
        float tex_w = 0.0f;
        float tex_h = 0.0f;
        if (texture && SDL_GetTextureSize(texture, &tex_w, &tex_h)) {
            frame_w = static_cast<int>(std::lround(tex_w));
            frame_h = static_cast<int>(std::lround(tex_h));
        }
    }

    if (frame_w <= 0 || frame_h <= 0) {
        return false;
    }

    const float remainder = safe_remainder_scale(asset);
    const int final_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(frame_w) * remainder)));
    const int final_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(frame_h) * remainder)));

    out.frame_w = frame_w;
    out.frame_h = frame_h;
    out.final_w = final_w;
    out.final_h = final_h;
    out.flip = asset.flipped ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
    return true;
}

std::optional<SpriteQuad> compute_sprite_quad(const Asset& asset,
                                              const FrameDimensions& dims,
                                              const WarpedScreenGrid& cam,
                                              float perspective_scale) {
    if (dims.final_w <= 0 || dims.final_h <= 0) {
        return std::nullopt;
    }

    const float half_width = static_cast<float>(dims.final_w) * 0.5f / perspective_scale;
    const float height = static_cast<float>(dims.final_h) / perspective_scale;
    const float world_x = asset.smoothed_translation_x();
    const float world_y = asset.smoothed_translation_y();
    const float base_z = asset.world_z_offset();

    SDL_FPoint screen_bl{};
    SDL_FPoint screen_br{};
    if (!cam.project_world_point(SDL_FPoint{world_x - half_width, world_y}, base_z, screen_bl) ||
        !cam.project_world_point(SDL_FPoint{world_x + half_width, world_y}, base_z, screen_br)) {
        return std::nullopt;
    }

    const float bottom_dx = screen_br.x - screen_bl.x;
    const float bottom_dy = screen_br.y - screen_bl.y;
    const float bottom_len = std::hypot(bottom_dx, bottom_dy);
    if (bottom_len < 1e-5f) {
        return std::nullopt;
    }

    const float aspect = (dims.frame_w > 0 && dims.frame_h > 0)
        ? static_cast<float>(dims.frame_h) / static_cast<float>(dims.frame_w)
        : static_cast<float>(dims.final_h) / static_cast<float>(std::max(1, dims.final_w));
    float screen_height = bottom_len * aspect;
    if (!std::isfinite(screen_height) || screen_height <= 0.0f) {
        screen_height = std::abs(screen_height);
        if (screen_height <= 0.0f) {
            screen_height = height;
        }
    }

    const float nx = -bottom_dy / bottom_len;
    const float ny = bottom_dx / bottom_len;

    const SDL_FPoint cand_tl_a{screen_bl.x + nx * screen_height, screen_bl.y + ny * screen_height};
    const SDL_FPoint cand_tr_a{screen_br.x + nx * screen_height, screen_br.y + ny * screen_height};
    const SDL_FPoint cand_tl_b{screen_bl.x - nx * screen_height, screen_bl.y - ny * screen_height};
    const SDL_FPoint cand_tr_b{screen_br.x - nx * screen_height, screen_br.y - ny * screen_height};

    const float avg_bottom_y = 0.5f * (screen_bl.y + screen_br.y);
    const float avg_top_a = 0.5f * (cand_tl_a.y + cand_tr_a.y);
    const float avg_top_b = 0.5f * (cand_tl_b.y + cand_tr_b.y);

    SpriteQuad quad{};
    if ((avg_top_a < avg_bottom_y && avg_top_a <= avg_top_b) ||
        (avg_top_a <= avg_top_b && !(avg_top_b < avg_bottom_y))) {
        quad.tl = cand_tl_a;
        quad.tr = cand_tr_a;
    } else {
        quad.tl = cand_tl_b;
        quad.tr = cand_tr_b;
    }
    quad.bl = screen_bl;
    quad.br = screen_br;
    return quad;
}

SDL_FPoint lerp_point(const SDL_FPoint& a, const SDL_FPoint& b, float t) {
    return SDL_FPoint{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

std::optional<std::pair<SDL_FPoint, double>> unproject_to_world_y(const SDL_FPoint& screen,
                                                                  double target_world_y,
                                                                  const WarpedScreenGrid& cam) {
    const world::CameraProjectionParams params = cam.projection_params();
    if (params.screen_width <= 0 || params.screen_height <= 0) {
        return std::nullopt;
    }

    const double zoom = std::max(1e-6, params.screen_zoom);
    const double ndc_x = (static_cast<double>(screen.x) / static_cast<double>(params.screen_width) * 2.0 - 1.0) / zoom;
    const double ndc_y = (1.0 - (static_cast<double>(screen.y) - params.screen_pan_y_px) / static_cast<double>(params.screen_height) * 2.0) / zoom;

    const double tan_fov_x = std::max(1e-6, params.tan_half_fov_x);
    const double tan_fov_y = std::max(1e-6, params.tan_half_fov_y);

    const Vec3 cam_pos{params.position_x, params.position_y, params.position_z};
    const Vec3 cam_forward{params.forward_x, params.forward_y, params.forward_z};
    const Vec3 cam_right{params.right_x, params.right_y, params.right_z};
    const Vec3 cam_up{params.up_x, params.up_y, params.up_z};

    const double denom = cam_forward.y + ndc_x * tan_fov_x * cam_right.y + ndc_y * tan_fov_y * cam_up.y;
    if (std::abs(denom) <= 1e-9) {
        return std::nullopt;
    }

    const double meters_scale = std::max(1e-6, params.meters_scale);
    const double target_y_meters = (target_world_y - params.anchor_world_y) * meters_scale;
    const double depth = (target_y_meters - cam_pos.y) / denom;
    if (!std::isfinite(depth) || depth <= params.near_plane || depth >= params.far_plane) {
        return std::nullopt;
    }

    const double cam_x = depth * ndc_x * tan_fov_x;
    const double cam_z = depth * ndc_y * tan_fov_y;
    const Vec3 world_meters{
        cam_pos.x + cam_forward.x * depth + cam_right.x * cam_x + cam_up.x * cam_z,
        cam_pos.y + cam_forward.y * depth + cam_right.y * cam_x + cam_up.y * cam_z,
        cam_pos.z + cam_forward.z * depth + cam_right.z * cam_x + cam_up.z * cam_z
    };

    const double inv_scale = 1.0 / meters_scale;
    SDL_FPoint world_px{
        static_cast<float>(world_meters.x * inv_scale + params.anchor_world_x),
        static_cast<float>(world_meters.y * inv_scale + params.anchor_world_y)
    };
    const double world_z = world_meters.z * inv_scale;
    return std::make_pair(world_px, world_z);
}

}  // namespace

namespace anchor_points {

float anchor_height_px(const Asset& asset) {
    const float runtime_height = asset.runtime_height_px();
    if (std::isfinite(runtime_height) && runtime_height > 0.0f) {
        return runtime_height;
    }
    const int h = asset.height();
    return (h > 0) ? static_cast<float>(h) : 0.0f;
}

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    PixelLockedAnchor result{};
    result.resolved.in_front = anchor.in_front;
    if (!asset.get_assets()) {
        result.resolved.missing = true;
        return result;
    }

    FrameDimensions dims{};
    if (!gather_frame_dimensions(asset, dims)) {
        result.resolved.missing = true;
        return result;
    }

    Assets* assets_owner = asset.get_assets();
    WarpedScreenGrid& cam = assets_owner->getView();
    world::WorldGrid& grid = assets_owner->world_grid();
    const world::GridPoint* owner_gp = asset.grid_point();
    const float perspective_scale = owner_gp ? std::max(0.0001f, owner_gp->perspective_scale) : 1.0f;
    const int resolution_layer = owner_gp ? owner_gp->resolution_layer() : asset.grid_resolution;

    auto quad_opt = compute_sprite_quad(asset, dims, cam, perspective_scale);
    if (!quad_opt.has_value()) {
        result.resolved.missing = true;
        return result;
    }
    const SpriteQuad& quad = *quad_opt;

    const float fx = std::clamp((static_cast<float>(anchor.texture_x) + 0.5f) / static_cast<float>(dims.frame_w), 0.0f, 1.0f);
    const float fz = std::clamp((static_cast<float>(anchor.texture_z) + 0.5f) / static_cast<float>(dims.frame_h), 0.0f, 1.0f);
    const float u = ((dims.flip & SDL_FLIP_HORIZONTAL) != 0) ? (1.0f - fx) : fx;

    const SDL_FPoint bottom = lerp_point(quad.bl, quad.br, u);
    const SDL_FPoint top = lerp_point(quad.tl, quad.tr, u);
    const SDL_FPoint screen_anchor = lerp_point(top, bottom, fz);
    result.screen_px = screen_anchor;

    const int base_world_y = asset.world_y();
    const int depth_offset = anchor.in_front ? 1 : -1;
    const double target_world_y = static_cast<double>(base_world_y + depth_offset);

    auto world_opt = unproject_to_world_y(screen_anchor, target_world_y, cam);
    if (!world_opt.has_value()) {
        result.resolved.missing = true;
        return result;
    }

    const SDL_FPoint world_px_f = world_opt->first;
    const int world_x = static_cast<int>(std::lround(world_px_f.x));
    const int world_y = static_cast<int>(std::lround(world_px_f.y));
    const int world_z = static_cast<int>(std::lround(world_opt->second));

    result.resolved.world_px = SDL_Point{world_x, world_y};
    result.resolved.world_z = world_z;
    result.resolved.resolution_layer = resolution_layer;
    result.resolved.in_front = anchor.in_front;
    result.resolved.missing = false;

    if (grid_policy == GridMaterialization::Ensure) {
        const world::GridKey key{world_x, world_y, world_z, resolution_layer};
        result.resolved.grid_point = &grid.find_or_create_grid_point(key);
    } else {
        const world::GridKey key{world_x, world_y, world_z, resolution_layer};
        result.resolved.grid_point = grid.find_grid_point_strict(key);
    }

    return result;
}

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy) {
    return resolve_pixel_locked_anchor(asset, anchor, grid_policy).resolved;
}

}

#endif
