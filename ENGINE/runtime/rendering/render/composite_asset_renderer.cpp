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
#include "utils/sdl_render_conversions.hpp"

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
constexpr Uint8 kOverlayAlphaRebuildThreshold = 8;
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
    std::uint64_t merge_success = 0;
    std::uint64_t merge_failure = 0;
    std::uint64_t skipped_zero_opacity = 0;
    std::uint64_t skipped_missing_texture = 0;
    std::uint64_t skipped_invalid_dimensions = 0;
};

DepthCueMergeTelemetry& depth_cue_merge_telemetry() {
    static DepthCueMergeTelemetry telemetry{};
    return telemetry;
}

void depth_cue_log_periodic_summary() {
    DepthCueMergeTelemetry& telemetry = depth_cue_merge_telemetry();
    const std::uint64_t completed_merges = telemetry.merge_success + telemetry.merge_failure;
    if (completed_merges == 0 || (completed_merges % 120ull) != 0ull) {
        return;
    }
    vibble::log::debug(
        "[DepthCueMerge] summary candidates=" + std::to_string(telemetry.candidates) +
        " fg=" + std::to_string(telemetry.selected_foreground) +
        " bg=" + std::to_string(telemetry.selected_background) +
        " ok=" + std::to_string(telemetry.merge_success) +
        " fail=" + std::to_string(telemetry.merge_failure) +
        " skip_missing_texture=" + std::to_string(telemetry.skipped_missing_texture) +
        " skip_zero_opacity=" + std::to_string(telemetry.skipped_zero_opacity) +
        " skip_invalid_dimensions=" + std::to_string(telemetry.skipped_invalid_dimensions));
}

void depth_cue_note_candidate() {
    ++depth_cue_merge_telemetry().candidates;
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

void depth_cue_note_skip_invalid_dimensions(const std::string& message) {
    ++depth_cue_merge_telemetry().skipped_invalid_dimensions;
    vibble::log::warn("[DepthCueMerge] skipped merge due to invalid dimensions: " + message);
}

void depth_cue_note_merge_result(bool success, const std::string& failure_message = std::string{}) {
    DepthCueMergeTelemetry& telemetry = depth_cue_merge_telemetry();
    if (success) {
        ++telemetry.merge_success;
    } else {
        ++telemetry.merge_failure;
        if (!failure_message.empty()) {
            vibble::log::warn("[DepthCueMerge] merge failed: " + failure_message);
        }
    }
    depth_cue_log_periodic_summary();
}
#else
void depth_cue_note_candidate() {}
void depth_cue_note_selection(depth_cue::OverlayLayer) {}
void depth_cue_note_skip_missing_texture() {}
void depth_cue_note_skip_zero_opacity() {}
void depth_cue_note_skip_invalid_dimensions(const std::string&) {}
void depth_cue_note_merge_result(bool, const std::string& = std::string{}) {}
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

    float signed_depth = 0.0f;
    bool depth_effects_enabled = false;
    depth_cue::DepthCueSettings depth_settings{};
    if (assets_) {
        const WarpedScreenGrid& cam = assets_->getView();
        const world::CameraProjectionParams projection = cam.projection_params();
        const float effective_world_z =
            static_cast<float>(asset->world_z()) + asset->world_z_offset() + asset->render_anchor_offset_z();
        signed_depth = depth_cue::depth_offset_from_world_z(
            effective_world_z,
            static_cast<float>(cam.anchor_world_z()),
            projection.forward_z);
        depth_effects_enabled = assets_->depth_effects_enabled();
        depth_settings = assets_->depth_cue_settings();
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

SDL_Texture* CompositeAssetRenderer::compose_depth_cue_merged_texture(
    SDL_Texture* base_texture,
    const SDL_Rect* base_src_rect,
    int base_frame_w,
    int base_frame_h,
    SDL_Texture* overlay_texture,
    Uint8 overlay_alpha,
    Uint8 base_alpha,
    depth_cue::OverlayLayer overlay_layer,
    SDL_Point* out_size) const {
    if (!renderer_ || !base_texture || !overlay_texture || overlay_alpha == 0) {
        depth_cue_note_merge_result(false, "missing renderer/base/overlay texture or zero overlay alpha");
        return nullptr;
    }

    const int frame_w = std::max(1, base_frame_w);
    const int frame_h = std::max(1, base_frame_h);

    int merged_w = 0;
    int merged_h = 0;
    const bool has_overlay_size = query_texture_size(overlay_texture, &merged_w, &merged_h);
    if (!has_overlay_size) {
        merged_w = safe_double_dimension(frame_w);
        merged_h = safe_double_dimension(frame_h);
    }
    const int expected_overlay_w = safe_double_dimension(frame_w);
    const int expected_overlay_h = safe_double_dimension(frame_h);
    if (has_overlay_size &&
        (merged_w != expected_overlay_w || merged_h != expected_overlay_h)) {
        vibble::log::warn("[DepthCueMerge] unexpected overlay size " +
                          std::to_string(merged_w) + "x" + std::to_string(merged_h) +
                          " for base " + std::to_string(frame_w) + "x" + std::to_string(frame_h));
    }

    auto compute_base_dst = [&](int canvas_w, int canvas_h) {
        const int anchor_x = canvas_w / 2;
        const int anchor_y = static_cast<int>(std::lround(static_cast<float>(canvas_h) * kOverlayAnchorUv.y));
        SDL_Rect base_dst{};
        base_dst.w = frame_w;
        base_dst.h = frame_h;
        base_dst.x = anchor_x - (base_dst.w / 2);
        base_dst.y = anchor_y - base_dst.h;
        return base_dst;
    };

    SDL_Rect base_dst = compute_base_dst(merged_w, merged_h);
    const bool base_out_of_bounds =
        base_dst.x < 0 ||
        base_dst.y < 0 ||
        (base_dst.x + base_dst.w) > merged_w ||
        (base_dst.y + base_dst.h) > merged_h;
    if (base_out_of_bounds) {
        const int expanded_w = std::max(merged_w, safe_double_dimension(frame_w));
        const int expanded_h = std::max(merged_h, safe_double_dimension(frame_h));
        if (expanded_w == merged_w && expanded_h == merged_h) {
            depth_cue_note_skip_invalid_dimensions(
                "canvas=" + std::to_string(merged_w) + "x" + std::to_string(merged_h) +
                " base=" + std::to_string(frame_w) + "x" + std::to_string(frame_h));
            depth_cue_note_merge_result(false, "base projection exceeded merged canvas");
            return nullptr;
        }
        vibble::log::warn("[DepthCueMerge] overlay canvas too small; expanding from " +
                          std::to_string(merged_w) + "x" + std::to_string(merged_h) +
                          " to " + std::to_string(expanded_w) + "x" + std::to_string(expanded_h));
        merged_w = expanded_w;
        merged_h = expanded_h;
        base_dst = compute_base_dst(merged_w, merged_h);
    }

    SDL_Texture* merged_texture = SDL_CreateTexture(renderer_,
                                                    static_cast<SDL_PixelFormat>(SDL_PIXELFORMAT_RGBA32),
                                                    SDL_TEXTUREACCESS_TARGET,
                                                    merged_w,
                                                    merged_h);
    if (!merged_texture) {
        depth_cue_note_merge_result(false, "SDL_CreateTexture failed for merged texture");
        return nullptr;
    }

    SDL_SetTextureBlendMode(merged_texture, SDL_BLENDMODE_BLEND);

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    if (!SDL_SetRenderTarget(renderer_, merged_texture)) {
        SDL_DestroyTexture(merged_texture);
        SDL_SetRenderTarget(renderer_, previous_target);
        depth_cue_note_merge_result(false, "SDL_SetRenderTarget failed for merged texture");
        return nullptr;
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);

    SDL_BlendMode overlay_prev_blend = SDL_BLENDMODE_BLEND;
    Uint8 overlay_prev_r = 255;
    Uint8 overlay_prev_g = 255;
    Uint8 overlay_prev_b = 255;
    Uint8 overlay_prev_a = 255;
    SDL_GetTextureBlendMode(overlay_texture, &overlay_prev_blend);
    SDL_GetTextureColorMod(overlay_texture, &overlay_prev_r, &overlay_prev_g, &overlay_prev_b);
    SDL_GetTextureAlphaMod(overlay_texture, &overlay_prev_a);

    SDL_SetTextureBlendMode(overlay_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(overlay_texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(overlay_texture, overlay_alpha);

    const SDL_Rect overlay_dst{0, 0, merged_w, merged_h};

    SDL_BlendMode base_prev_blend = SDL_BLENDMODE_BLEND;
    Uint8 base_prev_r = 255;
    Uint8 base_prev_g = 255;
    Uint8 base_prev_b = 255;
    Uint8 base_prev_a = 255;
    SDL_GetTextureBlendMode(base_texture, &base_prev_blend);
    SDL_GetTextureColorMod(base_texture, &base_prev_r, &base_prev_g, &base_prev_b);
    SDL_GetTextureAlphaMod(base_texture, &base_prev_a);

    SDL_SetTextureBlendMode(base_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(base_texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(base_texture, base_alpha);
    const bool draw_overlay_over_base = (overlay_layer == depth_cue::OverlayLayer::Foreground);
    bool overlay_ok = false;
    bool base_ok = false;
    if (draw_overlay_over_base) {
        base_ok = sdl_render::Texture(renderer_, base_texture, base_src_rect, &base_dst);
        overlay_ok = sdl_render::Texture(renderer_, overlay_texture, nullptr, &overlay_dst);
    } else {
        overlay_ok = sdl_render::Texture(renderer_, overlay_texture, nullptr, &overlay_dst);
        base_ok = sdl_render::Texture(renderer_, base_texture, base_src_rect, &base_dst);
    }

    SDL_SetTextureBlendMode(overlay_texture, overlay_prev_blend);
    SDL_SetTextureColorMod(overlay_texture, overlay_prev_r, overlay_prev_g, overlay_prev_b);
    SDL_SetTextureAlphaMod(overlay_texture, overlay_prev_a);

    SDL_SetTextureBlendMode(base_texture, base_prev_blend);
    SDL_SetTextureColorMod(base_texture, base_prev_r, base_prev_g, base_prev_b);
    SDL_SetTextureAlphaMod(base_texture, base_prev_a);

    SDL_SetRenderTarget(renderer_, previous_target);

    if (!overlay_ok || !base_ok) {
        SDL_DestroyTexture(merged_texture);
        depth_cue_note_merge_result(false, "failed drawing base/overlay texture into merged target");
        return nullptr;
    }

    if (out_size) {
        out_size->x = merged_w;
        out_size->y = merged_h;
    }

    depth_cue_note_merge_result(true);
    return merged_texture;
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

        bool use_overlay_merge = desired_signature.overlay_active &&
                                 desired_signature.overlay_texture != nullptr &&
                                 desired_signature.overlay_alpha > 0;

        SDL_Texture* render_texture = base_tex;
        int render_frame_w = frame_w;
        int render_frame_h = frame_h;
        int render_texture_w = texture_w;
        int render_texture_h = texture_h;
        std::optional<SDL_Rect> render_src_rect =
            has_src_rect ? std::optional<SDL_Rect>(src_rect) : std::nullopt;
        SDL_FPoint projection_anchor_uv = kBaseAnchorUv;
        Uint8 render_alpha = asset_alpha;

        if (use_overlay_merge) {
            SDL_Point merged_size{0, 0};
            SDL_Texture* merged_texture = compose_depth_cue_merged_texture(
                base_tex,
                has_src_rect ? &src_rect : nullptr,
                frame_w,
                frame_h,
                desired_signature.overlay_texture,
                desired_signature.overlay_alpha,
                asset_alpha,
                overlay_layer_from_signature(desired_signature.overlay_layer),
                &merged_size);
            if (merged_texture) {
                asset->set_composite_texture(merged_texture);
                render_texture = merged_texture;
                render_frame_w = std::max(1, merged_size.x);
                render_frame_h = std::max(1, merged_size.y);
                render_texture_w = render_frame_w;
                render_texture_h = render_frame_h;
                render_src_rect = std::nullopt;
                projection_anchor_uv = kOverlayAnchorUv;
                render_alpha = 255;
                final_w = safe_double_dimension(final_w);
                final_h = safe_double_dimension(final_h);
            } else {
                use_overlay_merge = false;
                asset->set_composite_texture(nullptr);
            }
        } else {
            asset->set_composite_texture(nullptr);
        }

        const SDL_Rect dest_rect = {
            static_cast<int>(std::lround(world_anchor_x)),
            static_cast<int>(std::lround(world_anchor_y)),
            final_w,
            final_h
        };

        add_render_object(render_texture,
                          dest_rect,
                          SDL_Color{255, 255, 255, render_alpha},
                          SDL_BLENDMODE_BLEND,
                          false,
                          base_angle,
                          std::nullopt,
                          base_flip,
                          SDL_Point{render_frame_w, render_frame_h},
                          SDL_Point{render_texture_w, render_texture_h},
                          world_anchor_z_offset,
                          render_src_rect,
                          projection_anchor_uv,
                          SDL_FPoint{world_anchor_x, world_anchor_y});

        if (use_overlay_merge) {
            asset->depth_cue_merge_applied_signature_ = desired_signature;
        } else {
            DepthCueMergeSignature applied_signature{};
            applied_signature.valid = true;
            applied_signature.base_texture = base_tex;
            asset->depth_cue_merge_applied_signature_ = applied_signature;
        }
    } else {
        asset->set_composite_texture(nullptr);
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

SDL_Texture* CompositeAssetRenderer::test_compose_depth_cue_merged_texture(
    SDL_Texture* base_texture,
    const SDL_Rect* base_src_rect,
    int base_frame_w,
    int base_frame_h,
    SDL_Texture* overlay_texture,
    Uint8 overlay_alpha,
    Uint8 base_alpha,
    std::uint8_t overlay_layer,
    SDL_Point* out_size) const {
    return compose_depth_cue_merged_texture(base_texture,
                                            base_src_rect,
                                            base_frame_w,
                                            base_frame_h,
                                            overlay_texture,
                                            overlay_alpha,
                                            base_alpha,
                                            overlay_layer_from_signature(overlay_layer),
                                            out_size);
}
#endif
