#include "composite_asset_renderer.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame_variant.hpp"
#include "core/AssetsManager.hpp"
#include "core/manifest/depth_cue_settings.hpp"
#include "gameplay/world/grid_point.hpp"
#include "rendering/render/depth_cue_overlay_math.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/warped_screen_grid.hpp"

#include <algorithm>
#include <cstddef>
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

std::optional<std::size_t> find_overlay_index(const Asset* asset) {
    if (!asset) {
        return std::nullopt;
    }
    for (std::size_t idx = 0; idx < asset->render_package.size(); ++idx) {
        if (asset->render_package[idx].is_depth_cue_overlay) {
            return idx;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> find_base_index(const Asset* asset) {
    if (!asset || asset->render_package.empty()) {
        return std::nullopt;
    }
    for (std::size_t idx = asset->render_package.size(); idx > 0; --idx) {
        const std::size_t candidate = idx - 1;
        if (!asset->render_package[candidate].is_depth_cue_overlay) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool almost_equal(float a, float b, float epsilon = 1.0e-4f) {
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return a == b;
    }
    return std::fabs(a - b) <= epsilon;
}

bool overlay_objects_equivalent(const RenderObject& lhs, const RenderObject& rhs) {
    return lhs.texture == rhs.texture &&
           lhs.screen_rect.x == rhs.screen_rect.x &&
           lhs.screen_rect.y == rhs.screen_rect.y &&
           lhs.screen_rect.w == rhs.screen_rect.w &&
           lhs.screen_rect.h == rhs.screen_rect.h &&
           almost_equal(lhs.world_anchor_x, rhs.world_anchor_x) &&
           almost_equal(lhs.world_anchor_y, rhs.world_anchor_y) &&
           lhs.color_mod.r == rhs.color_mod.r &&
           lhs.color_mod.g == rhs.color_mod.g &&
           lhs.color_mod.b == rhs.color_mod.b &&
           lhs.color_mod.a == rhs.color_mod.a &&
           lhs.blend_mode == rhs.blend_mode &&
           almost_equal(static_cast<float>(lhs.angle), static_cast<float>(rhs.angle), 1.0e-6f) &&
           lhs.flip == rhs.flip &&
           lhs.texture_w == rhs.texture_w &&
           lhs.texture_h == rhs.texture_h &&
           lhs.has_texture_size == rhs.has_texture_size &&
           almost_equal(lhs.world_z_offset, rhs.world_z_offset) &&
           lhs.has_src_rect == rhs.has_src_rect &&
           lhs.atlas_w == rhs.atlas_w &&
           lhs.atlas_h == rhs.atlas_h &&
           lhs.has_atlas_size == rhs.has_atlas_size &&
           almost_equal(lhs.projection_anchor_uv.x, rhs.projection_anchor_uv.x, 1.0e-6f) &&
           almost_equal(lhs.projection_anchor_uv.y, rhs.projection_anchor_uv.y, 1.0e-6f) &&
           lhs.is_depth_cue_overlay == rhs.is_depth_cue_overlay;
}

bool overlay_size_changed(const RenderObject& lhs, const RenderObject& rhs) {
    return lhs.screen_rect.w != rhs.screen_rect.w || lhs.screen_rect.h != rhs.screen_rect.h;
}

bool build_overlay_render_object(const RenderObject& base_object,
                                 SDL_Texture* overlay_texture,
                                 Uint8 overlay_alpha,
                                 RenderObject& out_overlay) {
    if (!overlay_texture || overlay_alpha == 0) {
        return false;
    }

    float overlay_tex_wf = 0.0f;
    float overlay_tex_hf = 0.0f;
    if (!SDL_GetTextureSize(overlay_texture, &overlay_tex_wf, &overlay_tex_hf)) {
        return false;
    }
    const int overlay_tex_w = std::max(1, static_cast<int>(std::lround(overlay_tex_wf)));
    const int overlay_tex_h = std::max(1, static_cast<int>(std::lround(overlay_tex_hf)));

    RenderObject overlay{};
    overlay.texture = overlay_texture;
    overlay.screen_rect = build_overlay_dest_rect_from_base(base_object);
    overlay.world_anchor_x = base_object.world_anchor_x;
    overlay.world_anchor_y = base_object.world_anchor_y;
    overlay.color_mod = SDL_Color{255, 255, 255, overlay_alpha};
    overlay.blend_mode = SDL_BLENDMODE_BLEND;
    overlay.angle = base_object.angle;
    overlay.center = SDL_Point{0, 0};
    overlay.use_custom_center = false;
    overlay.flip = base_object.flip;
    overlay.texture_w = overlay_tex_w;
    overlay.texture_h = overlay_tex_h;
    overlay.has_texture_size = true;
    overlay.world_z_offset = base_object.world_z_offset;
    overlay.has_src_rect = false;
    overlay.src_rect = SDL_Rect{0, 0, 0, 0};
    overlay.atlas_w = overlay_tex_w;
    overlay.atlas_h = overlay_tex_h;
    overlay.has_atlas_size = true;
    overlay.dimension_cache_texture = overlay_texture;
    overlay.projection_anchor_uv = kOverlayAnchorUv;
    overlay.mesh_dirty = true;
    overlay.is_depth_cue_overlay = true;

    out_overlay = overlay;
    return true;
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

    const bool overlay_bounds_changed = refresh_depth_cue_overlay(asset);
    if (overlay_bounds_changed) {
        calculate_local_bounds(asset);
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
                                  std::optional<SDL_FPoint> projection_anchor_uv = std::nullopt,
                                  std::optional<SDL_FPoint> world_anchor = std::nullopt) {
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
        obj.projection_anchor_uv = projection_anchor_uv.value_or(SDL_FPoint{0.5f, 1.0f});
        asset->render_package.push_back(obj);
    };

    SDL_Texture* base_tex = nullptr;
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

        const float world_anchor_x = asset->smoothed_translation_x() + asset->render_anchor_offset_x();
        const float world_anchor_y = asset->smoothed_translation_y() + asset->render_anchor_offset_y();
        const float world_anchor_z_offset =
            asset->world_z_offset() + asset->render_anchor_offset_z();
        SDL_Rect dest_rect = {
            static_cast<int>(std::lround(world_anchor_x)),
            static_cast<int>(std::lround(world_anchor_y)),
            final_w,
            final_h
        };
        const SDL_FlipMode base_flip = asset->effective_render_flip();
        const double base_angle = asset->effective_render_angle();
        const Uint8 asset_alpha = static_cast<Uint8>(std::lround(std::clamp(asset->smoothed_alpha(), 0.0f, 1.0f) * 255.0f));
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
                          has_src_rect ? std::optional<SDL_Rect>(src_rect) : std::nullopt,
                          SDL_FPoint{0.5f, 1.0f},
                          SDL_FPoint{world_anchor_x, world_anchor_y});

    }

    asset->mark_mesh_dirty();
    asset->clear_composite_dirty();
    calculate_local_bounds(asset);
}

bool CompositeAssetRenderer::remove_depth_cue_overlay_objects(Asset* asset) {
    if (!asset) {
        return false;
    }

    auto& package = asset->render_package;
    const auto new_end = std::remove_if(package.begin(),
                                        package.end(),
                                        [](const RenderObject& obj) { return obj.is_depth_cue_overlay; });
    if (new_end == package.end()) {
        return false;
    }

    package.erase(new_end, package.end());
    asset->mark_mesh_dirty();
    return true;
}

bool CompositeAssetRenderer::upsert_depth_cue_overlay_object(Asset* asset,
                                                             std::size_t base_index,
                                                             const RenderObject& desired_overlay) {
    if (!asset) {
        return false;
    }

    auto overlay_index_opt = find_overlay_index(asset);
    const std::size_t desired_index = std::min(base_index + 1, asset->render_package.size());

    if (!overlay_index_opt.has_value()) {
        asset->render_package.insert(asset->render_package.begin() + static_cast<std::ptrdiff_t>(desired_index),
                                     desired_overlay);
        asset->mark_mesh_dirty();
        return true;
    }

    const std::size_t overlay_index = *overlay_index_opt;
    const RenderObject& existing_overlay = asset->render_package[overlay_index];
    const bool bounds_changed = overlay_size_changed(existing_overlay, desired_overlay);
    const bool order_changed = (overlay_index != desired_index);
    if (!order_changed && overlay_objects_equivalent(existing_overlay, desired_overlay)) {
        return false;
    }

    asset->render_package.erase(asset->render_package.begin() + static_cast<std::ptrdiff_t>(overlay_index));
    std::size_t insertion_index = desired_index;
    if (overlay_index < insertion_index) {
        insertion_index -= 1;
    }
    asset->render_package.insert(asset->render_package.begin() + static_cast<std::ptrdiff_t>(insertion_index),
                                 desired_overlay);
    asset->mark_mesh_dirty();
    return bounds_changed;
}

bool CompositeAssetRenderer::refresh_depth_cue_overlay(Asset* asset) {
    if (!asset) {
        return false;
    }

    const std::optional<std::size_t> base_index_opt = find_base_index(asset);
    if (!base_index_opt.has_value() || !assets_ || !renderer_) {
        return remove_depth_cue_overlay_objects(asset);
    }

    const std::size_t base_index = *base_index_opt;
    const RenderObject base_object = asset->render_package[base_index];

    SDL_Texture* depth_cue_foreground = nullptr;
    SDL_Texture* depth_cue_background = nullptr;
    if (asset->current_frame) {
        const auto& variants = asset->current_frame->variants;
        if (!variants.empty()) {
            int variant_idx = asset->current_variant_index;
            variant_idx = std::clamp(variant_idx, 0, static_cast<int>(variants.size()) - 1);
            const FrameVariant& variant = variants[static_cast<std::size_t>(variant_idx)];
            depth_cue_foreground = variant.get_foreground_texture();
            depth_cue_background = variant.get_background_texture();
        }
    }

    const WarpedScreenGrid& cam = assets_->getView();
    const depth_cue::DepthCueSettings& depth_settings = assets_->depth_cue_settings();
    const float effective_world_z =
        static_cast<float>(asset->world_z()) + base_object.world_z_offset;
    const world::CameraProjectionParams projection = cam.projection_params();
    const float signed_depth = depth_cue::depth_offset_from_world_z(
        effective_world_z,
        static_cast<float>(cam.anchor_world_z()),
        projection.forward_z);

    const DepthCueOverlayDecision overlay_decision = decide_depth_cue_overlay(
        signed_depth,
        depth_settings,
        assets_->depth_effects_enabled(),
        depth_cue_foreground,
        depth_cue_background);

    const float base_alpha = std::clamp(static_cast<float>(base_object.color_mod.a) / 255.0f,
                                        0.0f,
                                        1.0f);
    const float overlay_opacity = std::clamp(overlay_decision.opacity * base_alpha, 0.0f, 1.0f);
    const Uint8 overlay_alpha = static_cast<Uint8>(std::lround(overlay_opacity * 255.0f));

    if (!overlay_decision.texture || overlay_alpha == 0) {
        return remove_depth_cue_overlay_objects(asset);
    }

    RenderObject desired_overlay{};
    if (!build_overlay_render_object(base_object, overlay_decision.texture, overlay_alpha, desired_overlay)) {
        return remove_depth_cue_overlay_objects(asset);
    }

    return upsert_depth_cue_overlay_object(asset, base_index, desired_overlay);
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

