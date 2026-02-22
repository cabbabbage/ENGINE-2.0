#include "AnchorPointResolver.hpp"

#include <algorithm>
#include <cmath>
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

FrameAnchorSample resolve_frame_anchor_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              AnchorDepthPolicy,
                                              GridMaterialization) {
    FrameAnchorSample sample{};
    sample.resolved.world_px = asset.world_point();
    sample.resolved.world_z = asset.world_z();
    sample.screen_px = SDL_FPoint{static_cast<float>(asset.world_x()), static_cast<float>(asset.world_y())};
    sample.resolved.in_front = anchor.in_front;
    sample.resolved.missing = false;
    return sample;
}

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    const auto sample = resolve_frame_anchor_sample(asset,
                                                    anchor,
                                                    anchor.in_front ? AnchorDepthPolicy::InFront : AnchorDepthPolicy::Behind,
                                                    grid_policy);
    return PixelLockedAnchor{sample.resolved, sample.screen_px};
}

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy) {
    return resolve_frame_anchor_sample(asset,
                                       anchor,
                                       anchor.in_front ? AnchorDepthPolicy::InFront : AnchorDepthPolicy::Behind,
                                       grid_policy).resolved;
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

float anchor_to_unit(int pixel, int dimension) {
    if (dimension <= 0) {
        return 0.5f;
    }
    return std::clamp((static_cast<float>(pixel) + 0.5f) / static_cast<float>(dimension), 0.0f, 1.0f);
}

SDL_FPoint compute_anchor_uv(const DisplacedAssetAnchorPoint& anchor, const FrameDimensions& dims) {
    const float frame_u = anchor_to_unit(anchor.texture_x, dims.frame_w);
    const float frame_v = anchor_to_unit(anchor.texture_y, dims.frame_h);
    const float render_u = ((dims.flip & SDL_FLIP_HORIZONTAL) != 0) ? (1.0f - frame_u) : frame_u;
    return SDL_FPoint{render_u, frame_v};
}

SDL_FPoint compute_anchor_screen_from_mesh(const Asset& asset,
                                           const FrameDimensions& dims,
                                           const SDL_FPoint& uv,
                                           const WarpedScreenGrid& cam,
                                           float perspective_scale) {
    const float world_x = asset.smoothed_translation_x();
    const float world_y = asset.smoothed_translation_y();
    const float safe_perspective = (std::isfinite(perspective_scale) && perspective_scale > 0.0f) ? perspective_scale : 1.0f;
    const float world_width = static_cast<float>(dims.final_w) / safe_perspective;
    const float world_height = static_cast<float>(dims.final_h) / safe_perspective;
    const float half_width = world_width * 0.5f;

    SDL_FPoint screen_bl{};
    SDL_FPoint screen_br{};
    cam.project_world_point(SDL_FPoint{world_x - half_width, world_y}, dims.world_z_offset, screen_bl);
    cam.project_world_point(SDL_FPoint{world_x + half_width, world_y}, dims.world_z_offset, screen_br);

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
        screen_tl.x + (screen_tr.x - screen_tl.x) * uv.x,
        screen_tl.y + (screen_tr.y - screen_tl.y) * uv.x};
    const SDL_FPoint bottom{
        screen_bl.x + (screen_br.x - screen_bl.x) * uv.x,
        screen_bl.y + (screen_br.y - screen_bl.y) * uv.x};
    return SDL_FPoint{top.x + (bottom.x - top.x) * uv.y, top.y + (bottom.y - top.y) * uv.y};
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
                                              AnchorDepthPolicy depth_policy,
                                              GridMaterialization grid_policy) {
    FrameAnchorSample sample{};
    sample.resolved.in_front = anchor.in_front;

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

    const SDL_FPoint uv = compute_anchor_uv(anchor, dims);
    sample.uv = uv;

    const world::GridPoint* owner_gp = asset.grid_point();
    const float perspective_scale = owner_gp ? std::max(0.0001f, owner_gp->perspective_scale) : 1.0f;
    const int resolution_layer = owner_gp ? owner_gp->resolution_layer() : asset.grid_resolution;

    const float world_x = asset.smoothed_translation_x();
    const float world_y = asset.smoothed_translation_y();
    const float safe_perspective = (std::isfinite(perspective_scale) && perspective_scale > 0.0f) ? perspective_scale : 1.0f;
    const float world_width = static_cast<float>(dims.final_w) / safe_perspective;
    const float world_height = static_cast<float>(dims.final_h) / safe_perspective;

    const float anchor_world_x = world_x + (uv.x - 0.5f) * world_width;
    const float anchor_world_y = world_y;
    const float anchor_world_z = dims.world_z_offset + (1.0f - uv.y) * world_height;

    int depth_delta = 0;
    if (depth_policy == AnchorDepthPolicy::InFront) {
        depth_delta = 1;
    } else if (depth_policy == AnchorDepthPolicy::Behind) {
        depth_delta = -1;
    }

    const int resolved_x = static_cast<int>(std::lround(anchor_world_x));
    const int resolved_y = static_cast<int>(std::lround(anchor_world_y)) + depth_delta;
    const int resolved_z = static_cast<int>(std::lround(anchor_world_z));

    sample.resolved.world_px = SDL_Point{resolved_x, resolved_y};
    sample.resolved.world_z = resolved_z;
    sample.resolved.resolution_layer = resolution_layer;
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
                                                       uv,
                                                       assets_owner->getView(),
                                                       perspective_scale);
    return sample;
}

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    const auto sample = resolve_frame_anchor_sample(asset,
                                                    anchor,
                                                    anchor.in_front ? AnchorDepthPolicy::InFront : AnchorDepthPolicy::Behind,
                                                    grid_policy);
    return PixelLockedAnchor{sample.resolved, sample.screen_px};
}

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy) {
    return resolve_frame_anchor_sample(asset,
                                       anchor,
                                       anchor.in_front ? AnchorDepthPolicy::InFront : AnchorDepthPolicy::Behind,
                                       grid_policy).resolved;
}

}

#endif
