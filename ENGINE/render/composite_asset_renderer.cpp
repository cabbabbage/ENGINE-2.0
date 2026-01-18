#include "composite_asset_renderer.hpp"
#include "asset/Asset.hpp"
#include "asset/animation.hpp"
#include "asset/animation_frame_variant.hpp"
#include "core/AssetsManager.hpp"
#include "world/world_grid.hpp"
#include "world/grid_point.hpp"
#include "render/render.hpp"
#include "render/warped_screen_grid.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

CompositeAssetRenderer::CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets)
    : renderer_(renderer), assets_(assets) {}

CompositeAssetRenderer::~CompositeAssetRenderer() {}

void CompositeAssetRenderer::update(Asset* asset,
                                    float flicker_time_seconds) {
    if (!asset) return;

    float package_scale = asset->smoothed_scale();
    if (!std::isfinite(package_scale) || package_scale <= 0.0f) {
        package_scale = 1.0f;
    }

    if (std::abs(asset->composite_scale_ - package_scale) > 0.001f) {
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
    if (!renderer_ || !asset) return;

    asset->render_package.clear();

    asset->composite_scale_ = package_scale;

    auto add_render_object = [&](SDL_Texture* tex,
                                 SDL_Rect rect,
                                 SDL_Color color = {255, 255, 255, 255},
                                 SDL_BlendMode blend = SDL_BLENDMODE_BLEND,
                                 bool apply_scale = true,
                                 double angle = 0.0,
                                 std::optional<SDL_Point> center = std::nullopt,
                                 SDL_RendererFlip flip = SDL_FLIP_NONE,
                                 std::optional<SDL_Point> texture_size = std::nullopt,
                                 float world_z_offset = 0.0f) {
        if (!tex) return;
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
        obj.world_z_offset = world_z_offset;
        asset->render_package.push_back(obj);
    };

    SDL_Texture* base_tex = nullptr;
    SDL_Texture* depth_cue_foreground = nullptr;
    SDL_Texture* depth_cue_background = nullptr;

    if (asset->info) {
        auto anim_it = asset->info->animations.find(asset->current_animation);
        if (anim_it != asset->info->animations.end()) {
            if (asset->current_frame) {
                const auto& variants = asset->current_frame->variants;
                if (!variants.empty()) {
                    int variant_idx = asset->current_variant_index;
                    variant_idx = std::clamp(variant_idx, 0, static_cast<int>(variants.size()) - 1);
                    const FrameVariant& variant = variants[static_cast<std::size_t>(variant_idx)];
                    base_tex = variant.get_base_texture();
                    depth_cue_foreground = variant.get_foreground_texture();
                    depth_cue_background = variant.get_background_texture();
                }
            }
        }
    }

    if (!base_tex) {
        base_tex = asset->get_current_frame();
    }

    if (base_tex) {
        int w, h;
        SDL_QueryTexture(base_tex, nullptr, nullptr, &w, &h);

        float remainder = asset->current_remaining_scale_adjustment;
        if (!std::isfinite(remainder) || remainder <= 0.0f) {
            remainder = 1.0f;
        }
        const float base_adjustment = remainder;
        int final_w = static_cast<int>(std::lround(static_cast<float>(w) * base_adjustment));
        int final_h = static_cast<int>(std::lround(static_cast<float>(h) * base_adjustment));
        final_w = std::max(1, final_w);
        final_h = std::max(1, final_h);

        SDL_Rect dest_rect = {
            static_cast<int>(std::lround(asset->smoothed_translation_x())),
            static_cast<int>(std::lround(asset->smoothed_translation_y())),
            final_w,
            final_h
        };
        SDL_RendererFlip base_flip = asset->flipped ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
        add_render_object(base_tex,
                          dest_rect,
                          SDL_Color{255, 255, 255, 255},
                          SDL_BLENDMODE_BLEND,
                          false,
                          0.0,
                          std::nullopt,
                          base_flip,
                          SDL_Point{w, h},
                          asset->world_z_offset());

        bool have_depth_cue_distance = false;
        float asset_distance = 0.0f;
        if (assets_) {
            const WarpedScreenGrid& cam = assets_->getView();
            if (const auto* gp = cam.grid_point_for_asset(asset)) {
                asset_distance = gp->distance_to_camera;
                have_depth_cue_distance = std::isfinite(asset_distance);
            }
        }

        SDL_Texture* overlay_texture = nullptr;
        float overlay_opacity = 0.0f;
        const float fg_distance = kDepthCueForegroundFullOpacityDistance;
        const float bg_distance = kDepthCueBackgroundFullOpacityDistance;
        if (have_depth_cue_distance && fg_distance < bg_distance) {
            const float midpoint = 0.5f * (fg_distance + bg_distance);
            if (asset_distance <= fg_distance) {
                overlay_texture = depth_cue_foreground;
                overlay_opacity = 1.0f;
            } else if (asset_distance >= bg_distance) {
                overlay_texture = depth_cue_background;
                overlay_opacity = 1.0f;
            } else if (asset_distance < midpoint) {
                overlay_texture = depth_cue_foreground;
                const float denom = midpoint - fg_distance;
                if (denom > 0.0f) {
                    overlay_opacity = 1.0f - (asset_distance - fg_distance) / denom;
                }
            } else {
                overlay_texture = depth_cue_background;
                const float denom = bg_distance - midpoint;
                if (denom > 0.0f) {
                    overlay_opacity = (asset_distance - midpoint) / denom;
                }
            }
            overlay_opacity = std::clamp(overlay_opacity, 0.0f, 1.0f);
        }

        if (overlay_texture != nullptr && overlay_opacity > 0.0f) {
            const Uint8 overlay_alpha = static_cast<Uint8>(std::lround(overlay_opacity * 255.0f));
            if (overlay_alpha > 0) {
                int overlay_tex_w = 0;
                int overlay_tex_h = 0;
                SDL_QueryTexture(overlay_texture, nullptr, nullptr, &overlay_tex_w, &overlay_tex_h);
                overlay_tex_w = std::max(1, overlay_tex_w);
                overlay_tex_h = std::max(1, overlay_tex_h);
                add_render_object(overlay_texture,
                                  dest_rect,
                                  SDL_Color{255, 255, 255, overlay_alpha},
                                  SDL_BLENDMODE_BLEND,
                                  false,
                                  0.0,
                                  std::nullopt,
                                  base_flip,
                                  SDL_Point{overlay_tex_w, overlay_tex_h},
                                  asset->world_z_offset());
            }
        }
    }

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

    bounds.x -= asset->pos.x;
    bounds.y -= asset->pos.y;

    asset->composite_bounds_local_ = bounds;
}
