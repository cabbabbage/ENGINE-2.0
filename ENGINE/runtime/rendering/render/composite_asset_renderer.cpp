#include "composite_asset_renderer.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame_variant.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

constexpr SDL_FPoint kBaseAnchorUv{0.5f, 1.0f};

bool query_texture_size(SDL_Texture* texture, int* out_w, int* out_h) {
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

} // namespace

CompositeAssetRenderer::CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets)
    : renderer_(renderer), assets_(assets) {}

CompositeAssetRenderer::~CompositeAssetRenderer() {}

void CompositeAssetRenderer::update(Asset* asset,
                                    float flicker_time_seconds) {
    if (!asset) {
        return;
    }

    float package_scale = asset->smoothed_scale();
    if (!std::isfinite(package_scale) || package_scale <= 0.0f) {
        package_scale = 1.0f;
    }

    if (std::abs(asset->composite_scale_ - package_scale) > 0.0001f) {
        asset->mark_composite_dirty();
    }

    if (asset->is_composite_dirty()) {
        regenerate_package(asset, flicker_time_seconds, package_scale);
    } else {
        asset->composite_scale_ = package_scale;
    }
}

void CompositeAssetRenderer::regenerate_package(Asset* asset,
                                                float flicker_time_seconds,
                                                float package_scale) {
    if (!renderer_ || !asset) {
        return;
    }

    (void)flicker_time_seconds;
    asset->render_package.clear();
    asset->composite_scale_ = package_scale;

    auto add_render_object = [&](SDL_Texture* tex,
                                 SDL_Rect rect,
                                 SDL_Color color = {255, 255, 255, 255},
                                 SDL_BlendMode blend = SDL_BLENDMODE_BLEND,
                                 bool apply_scale = true,
                                 double angle = 0.0,
                                 std::optional<SDL_Point> center = std::nullopt,
                                 SDL_FlipMode flip = SDL_FLIP_NONE,
                                 std::optional<SDL_Point> texture_size = std::nullopt,
                                 std::optional<SDL_Point> atlas_size = std::nullopt,
                                 float world_z_offset = 0.0f,
                                 std::optional<SDL_Rect> src_rect = std::nullopt,
                                 std::optional<SDL_FPoint> projection_anchor_uv = std::nullopt,
                                 std::optional<SDL_FPoint> world_anchor = std::nullopt) {
        if (!tex) {
            return;
        }
        if (apply_scale) {
            rect.w = static_cast<int>(std::lround(static_cast<float>(rect.w) * package_scale));
            rect.h = static_cast<int>(std::lround(static_cast<float>(rect.h) * package_scale));
            rect.w = std::max(1, rect.w);
            rect.h = std::max(1, rect.h);
        }

        SDL_Point c = {0, 0};
        bool custom = false;
        if (center.has_value()) {
            c = center.value();
            if (apply_scale) {
                c.x = static_cast<int>(std::lround(static_cast<float>(c.x) * package_scale));
                c.y = static_cast<int>(std::lround(static_cast<float>(c.y) * package_scale));
            }
            custom = true;
        }

        RenderObject obj{};
        obj.texture = tex;
        obj.screen_rect = rect;
        obj.world_anchor_x = world_anchor.has_value() ? world_anchor->x : static_cast<float>(rect.x);
        obj.world_anchor_y = world_anchor.has_value() ? world_anchor->y : static_cast<float>(rect.y);
        obj.color_mod = color;
        obj.blend_mode = blend;
        obj.angle = angle;
        obj.center = c;
        obj.use_custom_center = custom;
        obj.flip = flip;
        if (texture_size.has_value()) {
            obj.texture_w = texture_size->x;
            obj.texture_h = texture_size->y;
            obj.has_texture_size = (obj.texture_w > 0 && obj.texture_h > 0);
        }
        if (atlas_size.has_value()) {
            obj.atlas_w = atlas_size->x;
            obj.atlas_h = atlas_size->y;
            obj.has_atlas_size = (obj.atlas_w > 0 && obj.atlas_h > 0);
            obj.dimension_cache_texture = tex;
        }
        obj.world_z_offset = world_z_offset;
        if (src_rect.has_value()) {
            obj.has_src_rect = true;
            obj.src_rect = src_rect.value();
        }
        obj.projection_anchor_uv = projection_anchor_uv.value_or(kBaseAnchorUv);
        asset->render_package.push_back(obj);
    };

    SDL_Texture* base_tex = nullptr;
    const FrameVariant* selected_variant = nullptr;

    if (asset->info) {
        auto anim_it = asset->info->animations.find(asset->current_animation);
        if (anim_it != asset->info->animations.end() && asset->current_frame) {
            const auto& variants = asset->current_frame->variants;
            if (!variants.empty()) {
                int variant_idx = asset->current_variant_index;
                variant_idx = std::clamp(variant_idx, 0, static_cast<int>(variants.size()) - 1);
                const FrameVariant& variant = variants[static_cast<std::size_t>(variant_idx)];
                selected_variant = &variant;
                base_tex = variant.get_base_texture();
            }
        }
    }

    if (!base_tex) {
        base_tex = asset->get_current_frame();
    }

    if (base_tex) {
        int texture_w = 0;
        int texture_h = 0;
        query_texture_size(base_tex, &texture_w, &texture_h);

        SDL_Rect src_rect{0, 0, texture_w, texture_h};
        bool has_src_rect = false;
        int frame_w = texture_w;
        int frame_h = texture_h;
        if (selected_variant) {
            if (selected_variant->source_rect.w > 0 && selected_variant->source_rect.h > 0) {
                frame_w = selected_variant->source_rect.w;
                frame_h = selected_variant->source_rect.h;
                src_rect = selected_variant->source_rect;
            }
            has_src_rect = selected_variant->uses_atlas;
        }

        float remainder = asset->current_remaining_scale_adjustment;
        if (!std::isfinite(remainder) || remainder <= 0.0f) {
            remainder = 1.0f;
        }
        const float base_adjustment = remainder;
        int final_w = static_cast<int>(std::lround(static_cast<float>(frame_w) * base_adjustment));
        int final_h = static_cast<int>(std::lround(static_cast<float>(frame_h) * base_adjustment));
        final_w = std::max(1, final_w);
        final_h = std::max(1, final_h);

        const float world_anchor_x = asset->smoothed_translation_x() + asset->render_anchor_offset_x();
        const float world_anchor_y = asset->smoothed_translation_y() + asset->render_anchor_offset_y();
        const float world_anchor_z_offset =
            asset->world_z_offset() + asset->render_anchor_offset_z();
        const SDL_FlipMode base_flip = asset->effective_render_flip();
        const double base_angle = asset->effective_render_angle();
        const Uint8 asset_alpha = static_cast<Uint8>(std::lround(
            std::clamp(asset->smoothed_alpha(), 0.0f, 1.0f) * 255.0f));
        const std::optional<SDL_Rect> render_src_rect =
            has_src_rect ? std::optional<SDL_Rect>(src_rect) : std::nullopt;

        const SDL_Rect dest_rect = {
            static_cast<int>(std::lround(world_anchor_x)),
            static_cast<int>(std::lround(world_anchor_y)),
            final_w,
            final_h
        };

        add_render_object(base_tex,
                          dest_rect,
                          SDL_Color{255, 255, 255, asset_alpha},
                          SDL_BLENDMODE_BLEND,
                          false,
                          base_angle,
                          std::nullopt,
                          base_flip,
                          SDL_Point{frame_w, frame_h},
                          SDL_Point{texture_w, texture_h},
                          world_anchor_z_offset,
                          render_src_rect,
                          kBaseAnchorUv,
                          SDL_FPoint{world_anchor_x, world_anchor_y});
    }

    asset->mark_mesh_dirty();
    asset->clear_composite_dirty();
    calculate_local_bounds(asset);
}

void CompositeAssetRenderer::calculate_local_bounds(Asset* asset) {
    if (!asset || asset->render_package.empty()) {
        asset->composite_bounds_local_ = {0, 0, 0, 0};
        return;
    }

    SDL_Rect bounds = asset->render_package[0].screen_rect;

    for (size_t i = 1; i < asset->render_package.size(); ++i) {
        const SDL_Rect& rect = asset->render_package[i].screen_rect;
        int new_x = std::min(bounds.x, rect.x);
        int new_y = std::min(bounds.y, rect.y);
        int new_w = std::max(bounds.x + bounds.w, rect.x + rect.w) - new_x;
        int new_h = std::max(bounds.y + bounds.h, rect.y + rect.h) - new_y;
        bounds = {new_x, new_y, new_w, new_h};
    }

    bounds.x -= asset->world_x();
    bounds.y -= asset->world_y();

    asset->composite_bounds_local_ = bounds;
}
