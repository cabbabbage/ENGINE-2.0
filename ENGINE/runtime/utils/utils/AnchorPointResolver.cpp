#include "AnchorPointResolver.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

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

FrameAnchorSample resolve_frame_anchor_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization) {
    FrameAnchorSample sample{};
    sample.resolved.world_px = asset.world_xy_point();
    sample.resolved.world_z = asset.world_z();
    sample.resolved.depth_offset = anchor.depth_offset;
    sample.screen_px = SDL_FPoint{static_cast<float>(asset.world_x()), static_cast<float>(asset.world_y())};
    sample.resolved.source_texture_px = SDL_Point{anchor.texture_x, anchor.texture_y};
    sample.resolved.has_canonical_texture_source = true;
    sample.resolved.missing = false;
    return sample;
}

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    const auto sample = resolve_frame_anchor_sample(asset, anchor, grid_policy);
    return PixelLockedAnchor{sample.resolved, sample.screen_px};
}

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy) {
    return resolve_frame_anchor_sample(asset, anchor, grid_policy).resolved;
}

}

#else

struct FrameDimensions {
    int frame_w = 0;
    int frame_h = 0;
    int final_w = 0;
    int final_h = 0;
    SDL_FlipMode flip = SDL_FLIP_NONE;
    float world_z_offset = 0.0f;
};

struct AnchorFrameSample {
    SDL_FPoint uv{0.5f, 0.5f};
    SDL_Point source_px{0, 0};
};

struct WorldPoint3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

std::optional<WorldPoint3> camera_world_position(const Assets* assets_owner) {
    if (!assets_owner) {
        return std::nullopt;
    }

    const auto params = assets_owner->getView().projection_params();
    const double meters_scale = std::max(1e-6, params.meters_scale);
    if (!std::isfinite(meters_scale)) {
        return std::nullopt;
    }

    WorldPoint3 world_camera_position{};
    world_camera_position.x = static_cast<float>(params.position_x / meters_scale + params.anchor_world_x);
    world_camera_position.y = static_cast<float>(params.position_y / meters_scale + params.anchor_world_y);
    world_camera_position.z = static_cast<float>(params.position_z / meters_scale + params.anchor_world_z);

    if (!std::isfinite(world_camera_position.x) ||
        !std::isfinite(world_camera_position.y) ||
        !std::isfinite(world_camera_position.z)) {
        return std::nullopt;
    }

    return world_camera_position;
}

bool apply_depth_offset_along_camera_ray(const Assets* assets_owner,
                                         int depth_offset,
                                         const WorldPoint3& flat_relative_pixel_point,
                                         WorldPoint3& final_anchor_point) {
    final_anchor_point = flat_relative_pixel_point;
    if (depth_offset == 0) {
        return true;
    }

    const auto camera_position_opt = camera_world_position(assets_owner);
    if (!camera_position_opt.has_value()) {
        return false;
    }

    const WorldPoint3 camera_position = *camera_position_opt;
    const float camera_to_point_x = flat_relative_pixel_point.x - camera_position.x;
    const float camera_to_point_y = flat_relative_pixel_point.y - camera_position.y;
    const float camera_to_point_z = flat_relative_pixel_point.z - camera_position.z;

    const float ray_length_sq =
        camera_to_point_x * camera_to_point_x +
        camera_to_point_y * camera_to_point_y +
        camera_to_point_z * camera_to_point_z;
    if (ray_length_sq <= 1e-10f || !std::isfinite(ray_length_sq)) {
        return false;
    }

    const float inv_ray_length = 1.0f / std::sqrt(ray_length_sq);
    const float offset_amount = static_cast<float>(depth_offset);

    final_anchor_point.x = flat_relative_pixel_point.x + camera_to_point_x * inv_ray_length * offset_amount;
    final_anchor_point.y = flat_relative_pixel_point.y + camera_to_point_y * inv_ray_length * offset_amount;
    final_anchor_point.z = flat_relative_pixel_point.z + camera_to_point_z * inv_ray_length * offset_amount;

    return std::isfinite(final_anchor_point.x) &&
           std::isfinite(final_anchor_point.y) &&
           std::isfinite(final_anchor_point.z);
}

void assert_anchor_is_canonical_texture_pixel(const DisplacedAssetAnchorPoint& anchor) {
    if (anchor.texture_x < 0 || anchor.texture_y < 0) {
        throw std::runtime_error("Anchor resolver invariant failure: canonical texture pixel coordinates must be non-negative");
    }
}

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
    out.frame_w = frame_w;
    out.frame_h = frame_h;
    out.final_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(frame_w) * remainder)));
    out.final_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(frame_h) * remainder)));
    out.flip = asset.flipped ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
    out.world_z_offset = asset.world_z_offset();
    return true;
}

AnchorFrameSample compute_anchor_frame_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              const FrameDimensions& dims) {
    // Anchors in authored data are canonical texture pixels. If runtime is using a scaled
    // variant, map that canonical pixel into the active variant before UV conversion.
    const float variant_scale = (std::isfinite(asset.current_nearest_variant_scale) &&
                                 asset.current_nearest_variant_scale > 0.0f)
                                    ? asset.current_nearest_variant_scale
                                    : 1.0f;

    const float scaled_x_f = static_cast<float>(anchor.texture_x) * variant_scale;
    const float scaled_y_f = static_cast<float>(anchor.texture_y) * variant_scale;

    SDL_Point scaled_px{
        static_cast<int>(std::lround(scaled_x_f)),
        static_cast<int>(std::lround(scaled_y_f))
    };

    if (dims.frame_w > 0) {
        scaled_px.x = std::clamp(scaled_px.x, 0, dims.frame_w - 1);
    }
    if (dims.frame_h > 0) {
        scaled_px.y = std::clamp(scaled_px.y, 0, dims.frame_h - 1);
    }

    const SDL_FPoint uv = anchor_points::anchor_pixel_to_uv(scaled_px, dims.frame_w, dims.frame_h, dims.flip);
    return AnchorFrameSample{uv, SDL_Point{anchor.texture_x, anchor.texture_y}};
}

WorldPoint3 compute_flat_relative_pixel_point(const Asset& asset,
                                              const FrameDimensions& dims,
                                              const SDL_FPoint& uv,
                                              float perspective_scale) {
    const float safe_perspective =
        (std::isfinite(perspective_scale) && perspective_scale > 0.0f) ? perspective_scale : 1.0f;
    const float world_width = static_cast<float>(dims.final_w) / safe_perspective;
    const float world_height = static_cast<float>(dims.final_h) / safe_perspective;

    WorldPoint3 flat_relative_pixel_point{};
    flat_relative_pixel_point.x =
        asset.smoothed_translation_x() + (uv.x - 0.5f) * world_width;
    flat_relative_pixel_point.y =
        asset.smoothed_translation_y() + (0.5f - uv.y) * world_height;
    flat_relative_pixel_point.z = static_cast<float>(asset.world_z());
    return flat_relative_pixel_point;
}

SDL_FPoint compute_anchor_screen_from_mesh(const Asset& asset,
                                           const FrameDimensions& dims,
                                           const SDL_FPoint& mesh_uv,
                                           const WarpedScreenGrid& cam,
                                           float perspective_scale) {
    const float world_x = asset.smoothed_translation_x();
    const float world_y = asset.smoothed_translation_y();
    const float base_world_z = static_cast<float>(asset.world_z());
    const float safe_perspective =
        (std::isfinite(perspective_scale) && perspective_scale > 0.0f) ? perspective_scale : 1.0f;
    const float world_width = static_cast<float>(dims.final_w) / safe_perspective;
    const float world_height = static_cast<float>(dims.final_h) / safe_perspective;
    const float half_width = world_width * 0.5f;

    SDL_FPoint screen_bl{};
    SDL_FPoint screen_br{};
    const float base_depth = base_world_z + dims.world_z_offset;
    cam.project_world_point(SDL_FPoint{world_x - half_width, world_y}, base_depth, screen_bl);
    cam.project_world_point(SDL_FPoint{world_x + half_width, world_y}, base_depth, screen_br);

    const float bottom_dx = screen_br.x - screen_bl.x;
    const float bottom_dy = screen_br.y - screen_bl.y;
    const float bottom_len = std::hypot(bottom_dx, bottom_dy);
    if (bottom_len < 1e-5f) {
        return screen_bl;
    }

    const float aspect = (dims.frame_w > 0 && dims.frame_h > 0)
        ? static_cast<float>(dims.frame_h) / static_cast<float>(dims.frame_w)
        : static_cast<float>(dims.final_h) / static_cast<float>(std::max(1, dims.final_w));
    float screen_height = bottom_len * aspect;
    if (!std::isfinite(screen_height) || screen_height <= 0.0f) {
        screen_height = std::abs(screen_height);
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

    SDL_FPoint screen_tl{};
    SDL_FPoint screen_tr{};
    if (avg_top_a <= avg_top_b) {
        screen_tl = cand_tl_a;
        screen_tr = cand_tr_a;
    } else {
        screen_tl = cand_tl_b;
        screen_tr = cand_tr_b;
    }
    if ((avg_top_a < avg_bottom_y) != (avg_top_b < avg_bottom_y)) {
        if (avg_top_a < avg_bottom_y) {
            screen_tl = cand_tl_a;
            screen_tr = cand_tr_a;
        } else {
            screen_tl = cand_tl_b;
            screen_tr = cand_tr_b;
        }
    }

    const SDL_FPoint top{
        screen_tl.x + (screen_tr.x - screen_tl.x) * mesh_uv.x,
        screen_tl.y + (screen_tr.y - screen_tl.y) * mesh_uv.x};
    const SDL_FPoint bottom{
        screen_bl.x + (screen_br.x - screen_bl.x) * mesh_uv.x,
        screen_bl.y + (screen_br.y - screen_bl.y) * mesh_uv.x};
    return SDL_FPoint{top.x + (bottom.x - top.x) * mesh_uv.y, top.y + (bottom.y - top.y) * mesh_uv.y};
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

FrameAnchorSample resolve_frame_anchor_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    FrameAnchorSample sample{};
    sample.resolved.depth_offset = anchor.depth_offset;

    Assets* assets_owner = asset.get_assets();
    if (!assets_owner) {
        sample.resolved.missing = true;
        return sample;
    }

    FrameDimensions dims{};
    if (!gather_frame_dimensions(asset, dims)) {
        sample.resolved.missing = true;
        return sample;
    }

    assert_anchor_is_canonical_texture_pixel(anchor);

    const AnchorFrameSample anchor_sample = compute_anchor_frame_sample(asset, anchor, dims);
    sample.uv = anchor_sample.uv;

    const world::GridPoint* owner_gp = asset.grid_point();
    const float perspective_scale = owner_gp ? std::max(0.0001f, owner_gp->perspective_scale) : 1.0f;
    const int resolution_layer = owner_gp ? owner_gp->resolution_layer() : asset.grid_resolution;

    const WorldPoint3 flat_relative_pixel_point =
        compute_flat_relative_pixel_point(asset, dims, anchor_sample.uv, perspective_scale);
    WorldPoint3 final_anchor_point{};
    if (!apply_depth_offset_along_camera_ray(
            assets_owner,
            anchor.depth_offset,
            flat_relative_pixel_point,
            final_anchor_point)) {
        sample.resolved.missing = true;
        return sample;
    }

    const int resolved_x = static_cast<int>(std::lround(final_anchor_point.x));
    const int resolved_y = static_cast<int>(std::lround(final_anchor_point.y));
    const int resolved_z = static_cast<int>(std::lround(final_anchor_point.z));

    sample.resolved.world_px = SDL_Point{resolved_x, resolved_y};
    sample.resolved.world_z = resolved_z;
    sample.resolved.resolution_layer = resolution_layer;
    sample.resolved.source_texture_px = anchor_sample.source_px;
    sample.resolved.has_canonical_texture_source = true;
    sample.resolved.missing = false;

    world::WorldGrid& grid = assets_owner->world_grid();
    const world::GridKey key{resolved_x, resolved_y, resolved_z, resolution_layer};
    if (grid_policy == GridMaterialization::Ensure) {
        sample.resolved.grid_point = &grid.find_or_create_grid_point(key);
    } else {
        sample.resolved.grid_point = grid.find_grid_point_strict(key);
    }

    sample.screen_px = compute_anchor_screen_from_mesh(asset,
                                                       dims,
                                                       anchor_sample.uv,
                                                       assets_owner->getView(),
                                                       perspective_scale);
    return sample;
}

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    const auto sample = resolve_frame_anchor_sample(asset, anchor, grid_policy);
    return PixelLockedAnchor{sample.resolved, sample.screen_px};
}

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy) {
    return resolve_frame_anchor_sample(asset, anchor, grid_policy).resolved;
}

}

#endif
