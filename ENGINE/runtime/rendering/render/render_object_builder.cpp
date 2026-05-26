#include "rendering/render/render_object_builder.hpp"

#include <algorithm>
#include <cmath>

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation_frame.hpp"
#include "assets/asset/animation_frame_variant.hpp"
#include "rendering/render/render_object.hpp"

namespace render_build {
namespace {

constexpr SDL_FPoint kDefaultProjectionAnchorUv{0.5f, 1.0f};

bool query_texture_size_direct(SDL_Texture* texture, int* out_w, int* out_h) {
    if (!texture || !out_w || !out_h) {
        return false;
    }

    float wf = 0.0f;
    float hf = 0.0f;
    if (!SDL_GetTextureSize(texture, &wf, &hf)) {
        return false;
    }

    *out_w = std::max(1, static_cast<int>(std::lround(wf)));
    *out_h = std::max(1, static_cast<int>(std::lround(hf)));
    return true;
}

Uint32 compute_reprojection_identity(const Asset& asset) {
    const SDL_FlipMode flip = asset.effective_render_flip();
    const int flip_h = (flip & SDL_FLIP_HORIZONTAL) != 0 ? 1 : 0;
    const int flip_v = (flip & SDL_FLIP_VERTICAL) != 0 ? 1 : 0;
    const int angle_q = static_cast<int>(std::lround(asset.effective_render_angle() * 100.0));
    const int z_q = static_cast<int>(std::lround((asset.world_z_offset() + asset.render_anchor_offset_z()) * 1000.0f));
    const int scale_q = static_cast<int>(std::lround(asset.current_remaining_scale_adjustment * 1000.0f));
    const int world_y_q = asset.world_y();
    Uint32 hash = 2166136261u;
    auto mix = [&](Uint32 v) {
        hash ^= v;
        hash *= 16777619u;
    };
    mix(static_cast<Uint32>(flip_h));
    mix(static_cast<Uint32>(flip_v));
    mix(static_cast<Uint32>(angle_q));
    mix(static_cast<Uint32>(z_q));
    mix(static_cast<Uint32>(scale_q));
    mix(static_cast<Uint32>(world_y_q));
    return hash;
}

bool query_asset_frame_variant(Asset* asset, const FrameVariant*& out_variant, SDL_Texture*& out_texture) {
    out_variant = nullptr;
    out_texture = nullptr;
    if (!asset) {
        return false;
    }

    if (asset->info) {
        auto anim_it = asset->info->animations.find(asset->current_animation);
        if (anim_it != asset->info->animations.end() && asset->current_frame) {
            const auto& variants = asset->current_frame->variants;
            if (!variants.empty()) {
                const int variant_idx = std::clamp(asset->current_variant_index, 0, static_cast<int>(variants.size()) - 1);
                out_variant = &variants[static_cast<std::size_t>(variant_idx)];
                out_texture = out_variant->get_base_texture();
            }
        }
    }

    if (!out_texture) {
        out_texture = asset->get_current_frame();
    }
    return out_texture != nullptr;
}

} // namespace

bool refresh_direct_asset_render_cache(Asset* asset, DirectAssetRenderCacheRecord& cache_record) {
    cache_record = DirectAssetRenderCacheRecord{};
    if (!asset) {
        return false;
    }

    SDL_Texture* base_tex = nullptr;
    const FrameVariant* selected_variant = nullptr;
    if (!query_asset_frame_variant(asset, selected_variant, base_tex)) {
        return false;
    }

    bool has_src_rect = false;
    int texture_w = 0;
    int texture_h = 0;
    int frame_w = 0;
    int frame_h = 0;
    SDL_Rect src_rect{0, 0, 0, 0};
    if (selected_variant) {
        if (selected_variant->source_rect.w > 0 && selected_variant->source_rect.h > 0) {
            frame_w = selected_variant->source_rect.w;
            frame_h = selected_variant->source_rect.h;
            src_rect = selected_variant->source_rect;
        }
        has_src_rect = selected_variant->uses_atlas;
    }

    if (!has_src_rect && frame_w > 0 && frame_h > 0) {
        texture_w = frame_w;
        texture_h = frame_h;
    } else if (!has_src_rect && asset->cached_w > 0 && asset->cached_h > 0) {
        texture_w = asset->cached_w;
        texture_h = asset->cached_h;
        frame_w = asset->cached_w;
        frame_h = asset->cached_h;
        src_rect = SDL_Rect{0, 0, frame_w, frame_h};
    } else {
        if (!query_texture_size_direct(base_tex, &texture_w, &texture_h)) {
            return false;
        }
        if (frame_w <= 0 || frame_h <= 0) {
            frame_w = texture_w;
            frame_h = texture_h;
            src_rect = SDL_Rect{0, 0, frame_w, frame_h};
        }
    }

    cache_record.texture = base_tex;
    cache_record.atlas_w = texture_w;
    cache_record.atlas_h = texture_h;
    cache_record.has_atlas_size = (texture_w > 0 && texture_h > 0);
    cache_record.frame_w = frame_w;
    cache_record.frame_h = frame_h;
    cache_record.has_texture_size = (frame_w > 0 && frame_h > 0);
    cache_record.has_src_rect = has_src_rect;
    cache_record.src_rect = src_rect;
    cache_record.projection_anchor_uv = kDefaultProjectionAnchorUv;
    cache_record.blend_mode = SDL_BLENDMODE_BLEND;
    cache_record.frame_identity = asset->current_frame ? asset->current_frame->frame_index : -1;
    cache_record.variant_identity = asset->current_variant_index;
    cache_record.texture_identity = base_tex;
    cache_record.reprojection_identity = compute_reprojection_identity(*asset);
    return true;
}

Uint32 direct_asset_reprojection_identity(Asset* asset) {
    if (!asset) {
        return 0;
    }
    return compute_reprojection_identity(*asset);
}

bool build_direct_asset_render_object(Asset* asset,
                                      const DirectAssetRenderCacheRecord& cache_record,
                                      RenderObject& out_object) {
    out_object = RenderObject{};
    if (!asset || !cache_record.texture) {
        return false;
    }

    float remainder = asset->current_remaining_scale_adjustment;
    if (!std::isfinite(remainder) || remainder <= 0.0f) {
        remainder = 1.0f;
    }

    int final_w = static_cast<int>(std::lround(static_cast<float>(cache_record.frame_w) * remainder));
    int final_h = static_cast<int>(std::lround(static_cast<float>(cache_record.frame_h) * remainder));
    final_w = std::max(1, final_w);
    final_h = std::max(1, final_h);

    const float world_anchor_x = asset->smoothed_translation_x() + asset->render_anchor_offset_x();
    const float world_anchor_y = asset->smoothed_translation_y() + asset->render_anchor_offset_y();
    const float world_anchor_z_offset = asset->world_z_offset() + asset->render_anchor_offset_z();
    const SDL_FlipMode base_flip = asset->effective_render_flip();
    const double base_angle = asset->effective_render_angle();
    const Uint8 asset_alpha = static_cast<Uint8>(std::lround(
        std::clamp(asset->smoothed_alpha(), 0.0f, 1.0f) * 255.0f));

    out_object.texture = cache_record.texture;
    out_object.screen_rect = SDL_Rect{
        static_cast<int>(std::lround(world_anchor_x)),
        static_cast<int>(std::lround(world_anchor_y)),
        final_w,
        final_h
    };
    out_object.world_anchor_x = world_anchor_x;
    out_object.world_anchor_y = world_anchor_y;
    out_object.color_mod = SDL_Color{255, 255, 255, asset_alpha};
    out_object.blend_mode = SDL_BLENDMODE_BLEND;
    out_object.angle = base_angle;
    out_object.center = SDL_Point{0, 0};
    out_object.use_custom_center = false;
    out_object.flip = base_flip;
    out_object.texture_w = cache_record.frame_w;
    out_object.texture_h = cache_record.frame_h;
    out_object.has_texture_size = cache_record.has_texture_size;
    out_object.atlas_w = cache_record.atlas_w;
    out_object.atlas_h = cache_record.atlas_h;
    out_object.has_atlas_size = cache_record.has_atlas_size;
    out_object.dimension_cache_texture = cache_record.texture;
    out_object.world_z_offset = world_anchor_z_offset;
    out_object.projection_anchor_uv = cache_record.projection_anchor_uv;
    out_object.has_src_rect = cache_record.has_src_rect;
    out_object.src_rect = cache_record.src_rect;

    return true;
}

bool build_direct_asset_render_object(Asset* asset, RenderObject& out_object) {
    DirectAssetRenderCacheRecord cache_record{};
    if (!refresh_direct_asset_render_cache(asset, cache_record)) {
        out_object = RenderObject{};
        return false;
    }
    return build_direct_asset_render_object(asset, cache_record, out_object);
}

} // namespace render_build
