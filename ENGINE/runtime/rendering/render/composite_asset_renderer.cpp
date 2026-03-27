#include "composite_asset_renderer.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame_variant.hpp"
#include "core/AssetsManager.hpp"
#include "core/manifest/depth_cue_settings.hpp"
#include "rendering/render/depth_cue_overlay_math.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/warped_screen_grid.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>

namespace {

struct DepthCueOverlayDecision {
    SDL_Texture* texture = nullptr;
    float opacity = 0.0f;
};

// Overlay textures are generated on centered 2x canvases, so their
// bottom-center anchor sits at v=0.75 in texture UV space.
constexpr SDL_FPoint kOverlayAnchorUv{0.5f, 0.75f};

int safe_double_dimension(int value) {
    const int safe_value = std::max(1, value);
    const std::int64_t doubled = static_cast<std::int64_t>(safe_value) * 2;
    const std::int64_t max_value = static_cast<std::int64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(doubled, max_value));
}

SDL_Rect build_overlay_dest_rect_from_base(const RenderObject& base_object) {
    const int base_w = std::max(1, base_object.screen_rect.w);

    SDL_Rect overlay_rect{};
    overlay_rect.x = base_object.screen_rect.x;
    overlay_rect.y = base_object.screen_rect.y;
    overlay_rect.w = safe_double_dimension(base_w);
    overlay_rect.h = safe_double_dimension(std::max(1, base_object.screen_rect.h));
    return overlay_rect;
}

DepthCueOverlayDecision decide_depth_cue_overlay(float signed_depth,
                                                 const depth_cue::DepthCueSettings& settings,
                                                 bool depth_effects_enabled,
                                                 SDL_Texture* foreground_texture,
                                                 SDL_Texture* background_texture) {
    if (!depth_effects_enabled || !std::isfinite(signed_depth)) {
        return {};
    }
    const depth_cue::OverlayOpacityDecision opacity_decision =
        depth_cue::evaluate_overlay_opacity(signed_depth, settings);

    SDL_Texture* chosen_texture = nullptr;
    if (opacity_decision.layer == depth_cue::OverlayLayer::Background) {
        chosen_texture = background_texture;
    } else if (opacity_decision.layer == depth_cue::OverlayLayer::Foreground) {
        chosen_texture = foreground_texture;
    }
    if (!chosen_texture) {
        return {};
    }

    const float opacity = std::clamp(opacity_decision.opacity, 0.0f, 1.0f);
    if (!std::isfinite(opacity) || opacity <= 0.0f) {
        return {};
    }

    return DepthCueOverlayDecision{chosen_texture, opacity};
}

} // namespace

CompositeAssetRenderer::CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets)
    : renderer_(renderer), assets_(assets) {}

CompositeAssetRenderer::~CompositeAssetRenderer() {}

void CompositeAssetRenderer::update(Asset* asset,
                                    float flicker_time_seconds) {
    if (!asset) return;

    if (assets_) {
        const std::uint64_t depth_cue_version = assets_->depth_cue_settings_version();
        if (asset->composite_depth_cue_settings_version_ != depth_cue_version) {
            asset->composite_depth_cue_settings_version_ = depth_cue_version;
            asset->mark_composite_dirty();
        }
    }

    float package_scale = asset->smoothed_scale();
    if (!std::isfinite(package_scale) || package_scale <= 0.0f) {
        package_scale = 1.0f;
    }

    // Keep package regeneration sensitivity close to runtime scale epsilon so
    // camera-driven size adjustments stay visually in sync.
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
                                  SDL_FlipMode flip = SDL_FLIP_NONE,
                                  std::optional<SDL_Point> texture_size = std::nullopt,
                                  std::optional<SDL_Point> atlas_size = std::nullopt,
                                  float world_z_offset = 0.0f,
                                  std::optional<SDL_Rect> src_rect = std::nullopt,
                                  std::optional<SDL_FPoint> projection_anchor_uv = std::nullopt) {
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
        obj.projection_anchor_uv = projection_anchor_uv.value_or(SDL_FPoint{0.5f, 1.0f});
        asset->render_package.push_back(obj);
    };

    SDL_Texture* base_tex = nullptr;
    SDL_Texture* depth_cue_foreground = nullptr;
    SDL_Texture* depth_cue_background = nullptr;
    const FrameVariant* selected_variant = nullptr;

    if (asset->info) {
        auto anim_it = asset->info->animations.find(asset->current_animation);
        if (anim_it != asset->info->animations.end()) {
            if (asset->current_frame) {
                const auto& variants = asset->current_frame->variants;
                if (!variants.empty()) {
                    int variant_idx = asset->current_variant_index;
                    variant_idx = std::clamp(variant_idx, 0, static_cast<int>(variants.size()) - 1);
                    const FrameVariant& variant = variants[static_cast<std::size_t>(variant_idx)];
                    selected_variant = &variant;
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
        int texture_w = 0;
        int texture_h = 0;
        float texture_wf = 0.0f;
        float texture_hf = 0.0f;
        if (SDL_GetTextureSize(base_tex, &texture_wf, &texture_hf)) {
            texture_w = static_cast<int>(std::lround(texture_wf));
            texture_h = static_cast<int>(std::lround(texture_hf));
        }

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

        SDL_Rect dest_rect = {
            static_cast<int>(std::lround(asset->smoothed_translation_x())),
            static_cast<int>(std::lround(asset->smoothed_translation_y())),
            final_w,
            final_h
        };
        SDL_FlipMode base_flip = asset->flipped ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
        const Uint8 asset_alpha = static_cast<Uint8>(std::lround(std::clamp(asset->smoothed_alpha(), 0.0f, 1.0f) * 255.0f));
        add_render_object(base_tex,
                          dest_rect,
                          SDL_Color{255, 255, 255, asset_alpha},
                          SDL_BLENDMODE_BLEND,
                          false,
                          0.0,
                          std::nullopt,
                          base_flip,
                          SDL_Point{frame_w, frame_h},
                          SDL_Point{texture_w, texture_h},
                          asset->world_z_offset(),
                          has_src_rect ? std::optional<SDL_Rect>(src_rect) : std::nullopt);

        const RenderObject* const base_render_object =
            asset->render_package.empty() ? nullptr : &asset->render_package.back();
        if (base_render_object && assets_ && renderer_) {
            const WarpedScreenGrid& cam = assets_->getView();
            const depth_cue::DepthCueSettings& depth_settings = assets_->depth_cue_settings();
            const float effective_world_z =
                static_cast<float>(asset->world_z()) + base_render_object->world_z_offset;
            const float signed_depth = effective_world_z - static_cast<float>(cam.anchor_world_z());

            const DepthCueOverlayDecision overlay_decision = decide_depth_cue_overlay(
                signed_depth,
                depth_settings,
                assets_->depth_effects_enabled(),
                depth_cue_foreground,
                depth_cue_background);

            const float overlay_opacity = std::clamp(
                overlay_decision.opacity * (static_cast<float>(asset_alpha) / 255.0f),
                0.0f,
                1.0f);
            const Uint8 overlay_alpha = static_cast<Uint8>(std::lround(overlay_opacity * 255.0f));
            if (overlay_decision.texture && overlay_alpha > 0) {
                int overlay_tex_w = 0;
                int overlay_tex_h = 0;
                float overlay_tex_wf = 0.0f;
                float overlay_tex_hf = 0.0f;
                if (SDL_GetTextureSize(overlay_decision.texture, &overlay_tex_wf, &overlay_tex_hf)) {
                    overlay_tex_w = static_cast<int>(std::lround(overlay_tex_wf));
                    overlay_tex_h = static_cast<int>(std::lround(overlay_tex_hf));
                }
                overlay_tex_w = std::max(1, overlay_tex_w);
                overlay_tex_h = std::max(1, overlay_tex_h);

                const SDL_Rect overlay_rect = build_overlay_dest_rect_from_base(*base_render_object);
                add_render_object(overlay_decision.texture,
                                  overlay_rect,
                                  SDL_Color{255, 255, 255, overlay_alpha},
                                  SDL_BLENDMODE_BLEND,
                                  false,
                                  0.0,
                                  std::nullopt,
                                  base_flip,
                                  SDL_Point{overlay_tex_w, overlay_tex_h},
                                  SDL_Point{overlay_tex_w, overlay_tex_h},
                                  base_render_object->world_z_offset,
                                  std::nullopt,
                                  kOverlayAnchorUv);
            }
        }
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

