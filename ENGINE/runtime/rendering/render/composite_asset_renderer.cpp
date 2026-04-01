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
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace {

struct DepthCueOverlayDecision {
    depth_cue::OverlayLayer layer = depth_cue::OverlayLayer::None;
    SDL_Texture* texture = nullptr;
    float opacity = 0.0f;
};

constexpr SDL_FPoint kBaseAnchorUv{0.5f, 1.0f};
// Overlay textures are generated on centered 2x canvases, so their
// bottom-center anchor sits at v=0.75 in texture UV space.
constexpr SDL_FPoint kOverlayAnchorUv{0.5f, 0.75f};
constexpr Uint8 kOverlayAlphaRebuildThreshold = 1;
constexpr std::uint8_t kOverlayLayerNone = 0;
constexpr std::uint8_t kOverlayLayerForeground = 1;
constexpr std::uint8_t kOverlayLayerBackground = 2;

int safe_double_dimension(int value) {
    const int safe_value = std::max(1, value);
    const std::int64_t doubled = static_cast<std::int64_t>(safe_value) * 2;
    const std::int64_t max_value = static_cast<std::int64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(doubled, max_value));
}

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

DepthCueRenderData resolve_depth_cue_render_data(Asset* asset) {
    DepthCueRenderData out{};
    if (!asset) {
        return out;
    }

    const FrameVariant* selected_variant = nullptr;
    if (asset->info) {
        auto anim_it = asset->info->animations.find(asset->current_animation);
        if (anim_it != asset->info->animations.end() && asset->current_frame) {
            const auto& variants = asset->current_frame->variants;
            if (!variants.empty()) {
                int variant_idx = asset->current_variant_index;
                variant_idx = std::clamp(variant_idx, 0, static_cast<int>(variants.size()) - 1);
                selected_variant = &variants[static_cast<std::size_t>(variant_idx)];
            }
        }
    }

    if (selected_variant) {
        out.base_texture = selected_variant->get_base_texture();
        out.foreground_texture = selected_variant->get_foreground_texture();
        out.background_texture = selected_variant->get_background_texture();
        out.has_depth_cue = (out.foreground_texture != nullptr || out.background_texture != nullptr);
    }

    if (!out.base_texture) {
        out.base_texture = asset->get_current_frame();
    }

    return out;
}

std::uint8_t overlay_layer_to_signature(depth_cue::OverlayLayer layer) {
    if (layer == depth_cue::OverlayLayer::Foreground) {
        return kOverlayLayerForeground;
    }
    if (layer == depth_cue::OverlayLayer::Background) {
        return kOverlayLayerBackground;
    }
    return kOverlayLayerNone;
}

depth_cue::OverlayLayer overlay_layer_from_signature(std::uint8_t layer_signature) {
    if (layer_signature == kOverlayLayerForeground) {
        return depth_cue::OverlayLayer::Foreground;
    }
    if (layer_signature == kOverlayLayerBackground) {
        return depth_cue::OverlayLayer::Background;
    }
    return depth_cue::OverlayLayer::None;
}

#if !defined(NDEBUG)
struct DepthCueMergeTelemetry {
    std::uint64_t candidates = 0;
    std::uint64_t selected_foreground = 0;
    std::uint64_t selected_background = 0;
    std::uint64_t skipped_zero_opacity = 0;
    std::uint64_t skipped_missing_texture = 0;
};

DepthCueMergeTelemetry& depth_cue_merge_telemetry() {
    static DepthCueMergeTelemetry telemetry{};
    return telemetry;
}

void depth_cue_log_periodic_summary() {
    DepthCueMergeTelemetry& telemetry = depth_cue_merge_telemetry();
    const std::uint64_t completed_candidates = telemetry.candidates;
    if (completed_candidates == 0 || (completed_candidates % 120ull) != 0ull) {
        return;
    }
    vibble::log::debug(
        "[DepthCueMerge] summary candidates=" + std::to_string(telemetry.candidates) +
        " fg=" + std::to_string(telemetry.selected_foreground) +
        " bg=" + std::to_string(telemetry.selected_background) +
        " skip_missing_texture=" + std::to_string(telemetry.skipped_missing_texture) +
        " skip_zero_opacity=" + std::to_string(telemetry.skipped_zero_opacity));
}

void depth_cue_note_candidate() {
    ++depth_cue_merge_telemetry().candidates;
    depth_cue_log_periodic_summary();
}

void depth_cue_note_selection(depth_cue::OverlayLayer layer) {
    DepthCueMergeTelemetry& telemetry = depth_cue_merge_telemetry();
    if (layer == depth_cue::OverlayLayer::Foreground) {
        ++telemetry.selected_foreground;
    } else if (layer == depth_cue::OverlayLayer::Background) {
        ++telemetry.selected_background;
    }
}

void depth_cue_note_skip_missing_texture() {
    ++depth_cue_merge_telemetry().skipped_missing_texture;
}

void depth_cue_note_skip_zero_opacity() {
    ++depth_cue_merge_telemetry().skipped_zero_opacity;
}
#else
void depth_cue_note_candidate() {}
void depth_cue_note_selection(depth_cue::OverlayLayer) {}
void depth_cue_note_skip_missing_texture() {}
void depth_cue_note_skip_zero_opacity() {}
#endif

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

    return DepthCueOverlayDecision{opacity_decision.layer, chosen_texture, opacity};
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

    const DepthCueMergeSignature desired_merge_signature =
        evaluate_depth_cue_merge_signature(asset);
    asset->depth_cue_merge_desired_signature_ = desired_merge_signature;
    if (should_mark_composite_dirty_for_depth_cue_merge(asset, desired_merge_signature)) {
        asset->mark_composite_dirty();
    }

    if (asset->is_composite_dirty()) {
        regenerate_package(asset, flicker_time_seconds, package_scale);
    } else {
        asset->composite_scale_ = package_scale;
    }
}

DepthCueMergeSignature CompositeAssetRenderer::evaluate_depth_cue_merge_signature(Asset* asset) const {
    DepthCueMergeSignature signature{};
    if (!asset) {
        return signature;
    }

    const DepthCueRenderData render_data = resolve_depth_cue_render_data(asset);
    if (render_data.has_depth_cue) {
        depth_cue_note_candidate();
    }

    const Uint8 base_alpha = static_cast<Uint8>(std::lround(
        std::clamp(asset->smoothed_alpha(), 0.0f, 1.0f) * 255.0f));

    float signed_depth = asset->runtime_camera_metrics.effective_world_z_depth_offset;
    bool depth_effects_enabled = false;
    depth_cue::DepthCueSettings depth_settings{};
    if (assets_) {
        depth_effects_enabled = assets_->depth_effects_enabled();
        depth_settings = assets_->depth_cue_settings();
        const RuntimeCameraMetrics& metrics = asset->runtime_camera_metrics;
        const bool has_cached_camera_metrics =
            metrics.valid &&
            metrics.frame_id == assets_->frame_id() &&
            metrics.camera_state_version == assets_->getView().camera_state_version();
        if (!has_cached_camera_metrics) {
            const WarpedScreenGrid& cam = assets_->getView();
            const world::CameraProjectionParams projection = cam.projection_params();
            const float effective_world_z =
                static_cast<float>(asset->world_z()) + asset->world_z_offset() + asset->render_anchor_offset_z();
            signed_depth = depth_cue::depth_offset_from_world_z(
                effective_world_z,
                static_cast<float>(cam.anchor_world_z()),
                projection.forward_z);
        }
    }

    return build_depth_cue_merge_signature(render_data.base_texture,
                                           render_data.foreground_texture,
                                           render_data.background_texture,
                                           signed_depth,
                                           base_alpha,
                                           depth_effects_enabled,
                                           depth_settings);
}

DepthCueMergeSignature CompositeAssetRenderer::build_depth_cue_merge_signature(
    SDL_Texture* base_texture,
    SDL_Texture* foreground_texture,
    SDL_Texture* background_texture,
    float signed_depth,
    Uint8 base_alpha,
    bool depth_effects_enabled,
    const depth_cue::DepthCueSettings& settings) const {
    DepthCueMergeSignature signature{};
    signature.valid = true;
    signature.base_texture = base_texture;

    if (!base_texture || base_alpha == 0) {
        return signature;
    }

    const DepthCueOverlayDecision overlay_decision = decide_depth_cue_overlay(
        signed_depth,
        settings,
        depth_effects_enabled,
        foreground_texture,
        background_texture);

    if (!overlay_decision.texture) {
        if (foreground_texture || background_texture) {
            depth_cue_note_skip_missing_texture();
        }
        return signature;
    }
    depth_cue_note_selection(overlay_decision.layer);

    const float clamped_opacity = std::clamp(overlay_decision.opacity, 0.0f, 1.0f);
    const Uint8 overlay_alpha = static_cast<Uint8>(std::lround(
        std::clamp(clamped_opacity * static_cast<float>(base_alpha), 0.0f, 255.0f)));

    if (overlay_alpha == 0) {
        depth_cue_note_skip_zero_opacity();
        return signature;
    }

    signature.overlay_active = true;
    signature.overlay_texture = overlay_decision.texture;
    signature.overlay_layer = overlay_layer_to_signature(overlay_decision.layer);
    signature.overlay_alpha = overlay_alpha;
    return signature;
}

bool CompositeAssetRenderer::should_mark_composite_dirty_for_depth_cue_merge(
    const Asset* asset,
    const DepthCueMergeSignature& desired_signature) const {
    if (!asset || !desired_signature.valid) {
        return false;
    }

    const DepthCueMergeSignature& applied_signature = asset->depth_cue_merge_applied_signature_;
    if (!applied_signature.valid) {
        return true;
    }

    if (applied_signature.base_texture != desired_signature.base_texture) {
        return true;
    }

    if (applied_signature.overlay_active != desired_signature.overlay_active) {
        return true;
    }

    if (!desired_signature.overlay_active) {
        return false;
    }

    if (applied_signature.overlay_texture != desired_signature.overlay_texture) {
        return true;
    }

    if (applied_signature.overlay_layer != desired_signature.overlay_layer) {
        return true;
    }

    const int alpha_delta = std::abs(static_cast<int>(applied_signature.overlay_alpha) -
                                     static_cast<int>(desired_signature.overlay_alpha));
    return alpha_delta >= static_cast<int>(kOverlayAlphaRebuildThreshold);
}

void CompositeAssetRenderer::regenerate_package(Asset* asset,
                                                float flicker_time_seconds,
                                                float package_scale) {
    if (!renderer_ || !asset) return;

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
        obj.projection_anchor_uv = projection_anchor_uv.value_or(kBaseAnchorUv);
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

        DepthCueMergeSignature desired_signature = asset->depth_cue_merge_desired_signature_;
        if (!desired_signature.valid || desired_signature.base_texture != base_tex) {
            desired_signature = evaluate_depth_cue_merge_signature(asset);
            asset->depth_cue_merge_desired_signature_ = desired_signature;
        }

        bool overlay_active = desired_signature.overlay_active &&
                              desired_signature.overlay_texture != nullptr &&
                              desired_signature.overlay_alpha > 0;
        const depth_cue::OverlayLayer overlay_layer =
            overlay_layer_from_signature(desired_signature.overlay_layer);
        if (overlay_active && overlay_layer == depth_cue::OverlayLayer::None) {
            overlay_active = false;
        }

        const std::optional<SDL_Rect> render_src_rect =
            has_src_rect ? std::optional<SDL_Rect>(src_rect) : std::nullopt;

        const SDL_Rect dest_rect = {
            static_cast<int>(std::lround(world_anchor_x)),
            static_cast<int>(std::lround(world_anchor_y)),
            final_w,
            final_h
        };

        auto add_base_object = [&]() {
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
        };

        auto add_overlay_object = [&]() {
            if (!overlay_active) {
                return;
            }

            int overlay_texture_w = 0;
            int overlay_texture_h = 0;
            if (!query_texture_size(desired_signature.overlay_texture, &overlay_texture_w, &overlay_texture_h)) {
                overlay_texture_w = safe_double_dimension(frame_w);
                overlay_texture_h = safe_double_dimension(frame_h);
            }
            overlay_texture_w = std::max(1, overlay_texture_w);
            overlay_texture_h = std::max(1, overlay_texture_h);

            const SDL_Rect overlay_rect = {
                static_cast<int>(std::lround(world_anchor_x)),
                static_cast<int>(std::lround(world_anchor_y)),
                safe_double_dimension(final_w),
                safe_double_dimension(final_h)
            };

            add_render_object(desired_signature.overlay_texture,
                              overlay_rect,
                              SDL_Color{255, 255, 255, desired_signature.overlay_alpha},
                              SDL_BLENDMODE_BLEND,
                              false,
                              base_angle,
                              std::nullopt,
                              base_flip,
                              SDL_Point{overlay_texture_w, overlay_texture_h},
                              SDL_Point{overlay_texture_w, overlay_texture_h},
                              world_anchor_z_offset,
                              std::nullopt,
                              kOverlayAnchorUv,
                              SDL_FPoint{world_anchor_x, world_anchor_y});
        };

        // Base must render first so the depth-cue overlay renders in front.
        add_base_object();
        add_overlay_object();

        if (overlay_active) {
            asset->depth_cue_merge_applied_signature_ = desired_signature;
        } else {
            DepthCueMergeSignature applied_signature{};
            applied_signature.valid = true;
            applied_signature.base_texture = base_tex;
            asset->depth_cue_merge_applied_signature_ = applied_signature;
        }
    } else {
        asset->depth_cue_merge_applied_signature_ = asset->depth_cue_merge_desired_signature_;
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

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
DepthCueMergeSignature CompositeAssetRenderer::test_build_depth_cue_merge_signature(
    SDL_Texture* base_texture,
    SDL_Texture* foreground_texture,
    SDL_Texture* background_texture,
    float signed_depth,
    Uint8 base_alpha,
    bool depth_effects_enabled,
    const depth_cue::DepthCueSettings& settings) const {
    return build_depth_cue_merge_signature(base_texture,
                                           foreground_texture,
                                           background_texture,
                                           signed_depth,
                                           base_alpha,
                                           depth_effects_enabled,
                                           settings);
}

bool CompositeAssetRenderer::test_should_mark_composite_dirty_for_depth_cue_merge(
    const Asset* asset,
    const DepthCueMergeSignature& desired_signature) const {
    return should_mark_composite_dirty_for_depth_cue_merge(asset, desired_signature);
}

void CompositeAssetRenderer::test_regenerate_package_with_signature(
    Asset* asset,
    float package_scale,
    const DepthCueMergeSignature& desired_signature) {
    if (!asset) {
        return;
    }
    asset->depth_cue_merge_desired_signature_ = desired_signature;
    regenerate_package(asset, 0.0f, package_scale);
}
#endif
