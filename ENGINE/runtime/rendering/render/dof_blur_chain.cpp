#include "rendering/render/dof_blur_chain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "rendering/render/render_diagnostics.hpp"

namespace {

constexpr float kEffectEpsilon = 1.0e-4f;

// Circular lens blur tuning. This makes alpha/non-alpha smear into transparent
// space instead of stopping at transparent pixels.
constexpr int kMaxCircularBlurRings = 5;
constexpr int kCircleDirections = 12;
constexpr float kCircularBlurWeight = 0.72f;
constexpr float kZoomBlurWeight = 0.28f;
constexpr float kCircularBlurRadiusMultiplier = 0.72f;
constexpr float kZoomBlurScaleMultiplier = 2.65f;

struct TextureStateSnapshot {
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    Uint8 alpha_mod = 255;
    Uint8 color_mod_r = 255;
    Uint8 color_mod_g = 255;
    Uint8 color_mod_b = 255;
};

float sanitized_non_negative(float value) {
    return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
}

float safe_unit(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }

    return std::clamp(value, 0.0f, 1.0f);
}

float smoothstep01(float value) {
    const float t = safe_unit(value);
    return t * t * (3.0f - 2.0f * t);
}

float lerp_float(float a, float b, float t) {
    return a + (b - a) * safe_unit(t);
}

float depth_curve(int layer_distance, float normalized_distance) {
    if (layer_distance <= 0) {
        return 0.0f;
    }

    const float absolute_t =
        std::clamp((static_cast<float>(layer_distance) - 0.35f) / 4.75f, 0.0f, 1.0f);
    const float absolute_curve = std::pow(smoothstep01(absolute_t), 2.10f);
    const float normalized_curve = std::pow(smoothstep01(normalized_distance), 2.20f);

    return std::clamp(absolute_curve * 0.78f + normalized_curve * 0.22f, 0.0f, 1.0f);
}

SDL_FPoint screen_center_for_target(int width, int height) {
    return SDL_FPoint{
        static_cast<float>(std::max(1, width)) * 0.5f,
        static_cast<float>(std::max(1, height)) * 0.5f
    };
}

TextureStateSnapshot capture_texture_state(SDL_Texture* texture) {
    TextureStateSnapshot state{};

    if (!texture) {
        return state;
    }

    SDL_GetTextureBlendMode(texture, &state.blend_mode);
    SDL_GetTextureAlphaMod(texture, &state.alpha_mod);
    SDL_GetTextureColorMod(texture, &state.color_mod_r, &state.color_mod_g, &state.color_mod_b);

    return state;
}

void restore_texture_state(SDL_Texture* texture, const TextureStateSnapshot& state) {
    if (!texture) {
        return;
    }

    SDL_SetTextureBlendMode(texture, state.blend_mode);
    SDL_SetTextureAlphaMod(texture, state.alpha_mod);
    SDL_SetTextureColorMod(texture, state.color_mod_r, state.color_mod_g, state.color_mod_b);
}

SDL_BlendMode make_sum_blend_mode() {
    return SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_SRC_ALPHA,
                                      SDL_BLENDFACTOR_ONE,
                                      SDL_BLENDOPERATION_ADD,
                                      SDL_BLENDFACTOR_ONE,
                                      SDL_BLENDFACTOR_ONE,
                                      SDL_BLENDOPERATION_ADD);
}

void clear_texture_target(SDL_Renderer* renderer, SDL_Texture* target) {
    if (!renderer || !target) {
        return;
    }

    render_diagnostics::set_render_target(renderer, target);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
}

bool copy_texture_region(SDL_Renderer* renderer,
                         SDL_Texture* src,
                         SDL_Texture* dst,
                         const SDL_FRect* src_rect,
                         const SDL_FRect* dst_rect) {
    if (!renderer || !src || !dst) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);

    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);

    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const bool ok = render_diagnostics::render_texture(renderer, src, src_rect, dst_rect);

    restore_texture_state(src, src_state);
    render_diagnostics::set_render_target(renderer, previous_target);

    return ok;
}

void draw_weighted_offset_sample(SDL_Renderer* renderer,
                                 SDL_Texture* texture,
                                 int draw_w,
                                 int draw_h,
                                 float offset_x,
                                 float offset_y,
                                 float weight) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || weight <= 0.0f) {
        return;
    }

    const int alpha = std::clamp(static_cast<int>(std::lround(weight * 255.0f)), 0, 255);
    if (alpha <= 0) {
        return;
    }

    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(alpha));

    const SDL_FRect src_rect{
        0.0f,
        0.0f,
        static_cast<float>(draw_w),
        static_cast<float>(draw_h)
    };

    const SDL_FRect dst_rect{
        offset_x,
        offset_y,
        static_cast<float>(draw_w),
        static_cast<float>(draw_h)
    };

    (void)render_diagnostics::render_texture(renderer, texture, &src_rect, &dst_rect);
}

void draw_weighted_scaled_sample(SDL_Renderer* renderer,
                                 SDL_Texture* texture,
                                 int draw_w,
                                 int draw_h,
                                 const SDL_FPoint& optical_center,
                                 float scale,
                                 float weight) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || weight <= 0.0f) {
        return;
    }

    const int alpha = std::clamp(static_cast<int>(std::lround(weight * 255.0f)), 0, 255);
    if (alpha <= 0) {
        return;
    }

    const float safe_scale =
        std::clamp(scale, 0.001f, 1.0f + dof_blur_chain::radial_blur_tuning::kMaxScaleDelta);

    const float src_w = static_cast<float>(draw_w);
    const float src_h = static_cast<float>(draw_h);
    const float scaled_w = src_w * safe_scale;
    const float scaled_h = src_h * safe_scale;

    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(alpha));

    const SDL_FRect src_rect{0.0f, 0.0f, src_w, src_h};
    const SDL_FRect dst_rect{
        optical_center.x - optical_center.x * safe_scale,
        optical_center.y - optical_center.y * safe_scale,
        scaled_w,
        scaled_h
    };

    (void)render_diagnostics::render_texture(renderer, texture, &src_rect, &dst_rect);
}

bool prepare_damage_pulse_texture(SDL_Renderer* renderer,
                                  SDL_Texture* src,
                                  SDL_Texture* dst,
                                  int draw_w,
                                  int draw_h,
                                  float warp_px,
                                  float tint_strength,
                                  float phase) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0 || src == dst) {
        return false;
    }

    const float safe_warp =
        std::clamp(std::abs(warp_px), 0.0f, dof_blur_chain::damage_pulse_tuning::kMaxWarpPx);
    const float safe_tint =
        std::clamp(tint_strength, 0.0f, dof_blur_chain::damage_pulse_tuning::kMaxTintStrength);

    if (safe_warp <= kEffectEpsilon && safe_tint <= kEffectEpsilon) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);

    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);

    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const float phase_sign = std::sin(phase) >= 0.0f ? 1.0f : -1.0f;
    const float side_offset_x = safe_warp * phase_sign;
    const float side_offset_y = safe_warp * 0.22f * phase_sign;

    draw_weighted_offset_sample(renderer, src, draw_w, draw_h, side_offset_x, side_offset_y, 0.28f);
    draw_weighted_offset_sample(renderer, src, draw_w, draw_h, -side_offset_x * 0.66f, -side_offset_y * 0.66f, 0.22f);
    draw_weighted_offset_sample(renderer, src, draw_w, draw_h, 0.0f, 0.0f, 0.50f);

    if (safe_tint > kEffectEpsilon) {
        const Uint8 tint_alpha = static_cast<Uint8>(std::clamp(
            static_cast<int>(std::lround(safe_tint * 255.0f)),
            0,
            255));

        const float green_blue_scale = std::clamp(1.0f - safe_tint * 0.78f, 0.0f, 1.0f);
        const Uint8 green_blue = static_cast<Uint8>(std::clamp(
            static_cast<int>(std::lround(green_blue_scale * 255.0f)),
            0,
            255));

        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, tint_alpha);
        SDL_SetTextureColorMod(src, 255, green_blue, green_blue);
        (void)render_diagnostics::render_texture(renderer, src, nullptr, nullptr);
    }

    restore_texture_state(src, src_state);
    render_diagnostics::set_render_target(renderer, previous_target);

    return true;
}

std::uint32_t hash_layer(int layer_id, int salt) {
    std::uint32_t h = 2166136261u;

    auto mix = [&h](std::uint32_t value) {
        h ^= value;
        h *= 16777619u;
    };

    mix(static_cast<std::uint32_t>(layer_id * 73856093));
    mix(static_cast<std::uint32_t>(salt * 19349663));
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;

    return h;
}

float hash_unit(int layer_id, int salt) {
    return static_cast<float>(hash_layer(layer_id, salt) & 0xFFFFu) / 65535.0f;
}

float wrapped_offset(float value, float period) {
    if (period <= 0.0f) {
        return 0.0f;
    }

    float out = std::fmod(value, period);
    if (out < 0.0f) {
        out += period;
    }

    return out;
}

float dust_layer_parallax(int layer_distance,
                          float depth_t,
                          bool foreground_layer,
                          bool background_seed) {
    if (background_seed) {
        return dof_blur_chain::atmospheric_dust_tuning::kBackgroundParallaxFar;
    }

    if (layer_distance <= 0) {
        return dof_blur_chain::atmospheric_dust_tuning::kFocusParallax;
    }

    if (foreground_layer) {
        return lerp_float(dof_blur_chain::atmospheric_dust_tuning::kForegroundParallaxNear,
                          dof_blur_chain::atmospheric_dust_tuning::kForegroundParallaxFar,
                          depth_t);
    }

    return lerp_float(dof_blur_chain::atmospheric_dust_tuning::kBackgroundParallaxNear,
                      dof_blur_chain::atmospheric_dust_tuning::kBackgroundParallaxFar,
                      depth_t);
}

bool draw_tiled_dust_texture(SDL_Renderer* renderer,
                             SDL_Texture* dust,
                             int width,
                             int height,
                             float tile_size,
                             float offset_x,
                             float offset_y,
                             float alpha) {
    if (!renderer || !dust || width <= 0 || height <= 0 || tile_size <= 4.0f || alpha <= 0.0f) {
        return true;
    }

    const int alpha_i = std::clamp(static_cast<int>(std::lround(alpha * 255.0f)), 0, 255);
    if (alpha_i <= 0) {
        return true;
    }

    const TextureStateSnapshot state = capture_texture_state(dust);

    SDL_SetTextureBlendMode(dust, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(dust, static_cast<Uint8>(alpha_i));
    SDL_SetTextureColorMod(dust, 255, 255, 255);

    const float wrapped_x = wrapped_offset(offset_x, tile_size);
    const float wrapped_y = wrapped_offset(offset_y, tile_size);

    const float start_x = -wrapped_x - tile_size;
    const float start_y = -wrapped_y - tile_size;
    const float end_x = static_cast<float>(width) + tile_size * 2.0f;
    const float end_y = static_cast<float>(height) + tile_size * 2.0f;

    bool ok = true;

    for (float y = start_y; y <= end_y; y += tile_size) {
        for (float x = start_x; x <= end_x; x += tile_size) {
            const SDL_FRect dst{x, y, tile_size, tile_size};
            if (!render_diagnostics::render_texture(renderer, dust, nullptr, &dst)) {
                ok = false;
                break;
            }
        }

        if (!ok) {
            break;
        }
    }

    restore_texture_state(dust, state);
    return ok;
}

bool add_layer_atmospheric_dust(SDL_Renderer* renderer,
                                SDL_Texture* src,
                                SDL_Texture* dst,
                                const std::vector<SDL_Texture*>& dust_frames,
                                int width,
                                int height,
                                int depth_layer,
                                int focus_depth_layer,
                                int max_layer_distance,
                                float time_seconds,
                                bool foreground_layer,
                                bool background_seed,
                                dof_blur_chain::DustAnchor dust_anchor) {
    if (!renderer || !src || !dst || width <= 0 || height <= 0 || src == dst) {
        return false;
    }

    if (!copy_texture_region(renderer, src, dst, nullptr, nullptr)) {
        return false;
    }

    if (!dof_blur_chain::atmospheric_dust_tuning::kEnabled || dust_frames.empty()) {
        return true;
    }

    const int layer_distance = std::abs(depth_layer - focus_depth_layer);

    if (!background_seed &&
        layer_distance > dof_blur_chain::atmospheric_dust_tuning::kMaxDustLayerDistance) {
        return true;
    }

    const float normalized_distance =
        max_layer_distance > 0
            ? std::clamp(static_cast<float>(layer_distance) / static_cast<float>(max_layer_distance), 0.0f, 1.0f)
            : 0.0f;

    const float depth_t = depth_curve(layer_distance, normalized_distance);

    float tile_scale = dof_blur_chain::atmospheric_dust_tuning::kFocusTileScale;

    if (background_seed) {
        tile_scale = dof_blur_chain::atmospheric_dust_tuning::kBackgroundFarTileScale;
    } else if (layer_distance > 0 && foreground_layer) {
        tile_scale = lerp_float(dof_blur_chain::atmospheric_dust_tuning::kForegroundNearTileScale,
                                dof_blur_chain::atmospheric_dust_tuning::kForegroundFarTileScale,
                                depth_t);
    } else if (layer_distance > 0) {
        tile_scale = lerp_float(dof_blur_chain::atmospheric_dust_tuning::kBackgroundNearTileScale,
                                dof_blur_chain::atmospheric_dust_tuning::kBackgroundFarTileScale,
                                depth_t);
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    render_diagnostics::set_render_target(renderer, dst);

    const std::size_t frame_count = dust_frames.size();
    const float phase =
        std::max(0.0f, time_seconds) * dof_blur_chain::atmospheric_dust_tuning::kAnimationFps +
        hash_unit(depth_layer, 41) * static_cast<float>(frame_count);

    const int frame0 =
        static_cast<int>(std::floor(phase)) % static_cast<int>(frame_count);
    const int frame1 = (frame0 + 1) % static_cast<int>(frame_count);
    const float frame_mix = phase - std::floor(phase);

    SDL_Texture* dust0 = dust_frames[static_cast<std::size_t>(frame0)];
    SDL_Texture* dust1 = dust_frames[static_cast<std::size_t>(frame1)];

    float dust_w = 0.0f;
    float dust_h = 0.0f;
    if (!dust0 || !SDL_GetTextureSize(dust0, &dust_w, &dust_h) || dust_w <= 0.0f || dust_h <= 0.0f) {
        render_diagnostics::set_render_target(renderer, previous_target);
        return true;
    }

    const float base_tile_size = std::max(16.0f, std::min(dust_w, dust_h));
    const float tile_size =
        std::clamp(base_tile_size * tile_scale,
                   dof_blur_chain::atmospheric_dust_tuning::kMinTilePx,
                   dof_blur_chain::atmospheric_dust_tuning::kMaxTilePx);

    const float parallax = dust_layer_parallax(layer_distance, depth_t, foreground_layer, background_seed);
    const float pixels_per_world =
        std::max(0.001f, dust_anchor.pixels_per_world_unit) *
        dof_blur_chain::atmospheric_dust_tuning::kWorldAnchorPixelScale;

    const float world_offset_x = dust_anchor.world_x * pixels_per_world * parallax;
    const float world_offset_z = dust_anchor.world_z * pixels_per_world * parallax;

    const float base_x = hash_unit(depth_layer, 11) * tile_size;
    const float base_y = hash_unit(depth_layer, 17) * tile_size;

    const float offset_x = base_x + world_offset_x;
    const float offset_y = base_y + world_offset_z;

    constexpr float full_alpha = dof_blur_chain::atmospheric_dust_tuning::kDrawAlpha;

    const bool ok0 = draw_tiled_dust_texture(renderer,
                                             dust0,
                                             width,
                                             height,
                                             tile_size,
                                             offset_x,
                                             offset_y,
                                             full_alpha * (1.0f - frame_mix));

    const bool ok1 = draw_tiled_dust_texture(renderer,
                                             dust1,
                                             width,
                                             height,
                                             tile_size,
                                             offset_x,
                                             offset_y,
                                             full_alpha * frame_mix);

    const float secondary_tile =
        std::clamp(tile_size * dof_blur_chain::atmospheric_dust_tuning::kSecondaryPassScale,
                   dof_blur_chain::atmospheric_dust_tuning::kMinTilePx,
                   dof_blur_chain::atmospheric_dust_tuning::kMaxTilePx);

    const bool ok2 = draw_tiled_dust_texture(renderer,
                                             dust0,
                                             width,
                                             height,
                                             secondary_tile,
                                             offset_x * -0.37f + tile_size * hash_unit(depth_layer, 23),
                                             offset_y * 0.29f + tile_size * hash_unit(depth_layer, 29),
                                             full_alpha);

    const float tertiary_tile =
        std::clamp(tile_size * dof_blur_chain::atmospheric_dust_tuning::kTertiaryPassScale,
                   dof_blur_chain::atmospheric_dust_tuning::kMinTilePx,
                   dof_blur_chain::atmospheric_dust_tuning::kMaxTilePx);

    const bool ok3 = draw_tiled_dust_texture(renderer,
                                             dust1,
                                             width,
                                             height,
                                             tertiary_tile,
                                             offset_x * 0.71f + tile_size * hash_unit(depth_layer, 31),
                                             offset_y * -0.44f + tile_size * hash_unit(depth_layer, 37),
                                             full_alpha);

    render_diagnostics::set_render_target(renderer, previous_target);
    return ok0 && ok1 && ok2 && ok3;
}

int circular_blur_ring_count(float radius_px) {
    if (radius_px <= kEffectEpsilon) {
        return 0;
    }

    if (radius_px <= 4.0f) {
        return 2;
    }

    if (radius_px <= 9.0f) {
        return 3;
    }

    if (radius_px <= 18.0f) {
        return 4;
    }

    return kMaxCircularBlurRings;
}

int zoom_blur_sample_count(float radius_px) {
    if (radius_px <= kEffectEpsilon) {
        return 0;
    }

    return std::clamp(
        dof_blur_chain::radial_blur_tuning::kMinSamples +
            static_cast<int>(std::ceil(std::sqrt(radius_px) *
                                       dof_blur_chain::radial_blur_tuning::kSamplesPerSqrtRadius)),
        dof_blur_chain::radial_blur_tuning::kMinSamples,
        dof_blur_chain::radial_blur_tuning::kMaxSamples);
}

bool apply_fast_radial_zoom_blur(SDL_Renderer* renderer,
                                 SDL_Texture* src,
                                 SDL_Texture* dst,
                                 int draw_w,
                                 int draw_h,
                                 float radial_radius_px,
                                 SDL_BlendMode sum_blend) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0 || src == dst) {
        return false;
    }

    if (radial_radius_px <= kEffectEpsilon) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);

    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);

    SDL_SetTextureBlendMode(src, sum_blend);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const SDL_FPoint center = screen_center_for_target(draw_w, draw_h);
    const float max_dim = static_cast<float>(std::max(draw_w, draw_h));
    const float normalized_radius =
        std::clamp(radial_radius_px / std::max(1.0f, max_dim), 0.0f, 1.0f);

    const int ring_count = circular_blur_ring_count(radial_radius_px);
    const int zoom_count = zoom_blur_sample_count(radial_radius_px);

    const float circular_radius_px = radial_radius_px * kCircularBlurRadiusMultiplier;
    const float max_zoom_scale_delta =
        std::min(dof_blur_chain::radial_blur_tuning::kMaxScaleDelta,
                 normalized_radius * kZoomBlurScaleMultiplier);

    float total_raw_weight = 1.0f;

    std::array<float, kMaxCircularBlurRings> ring_weights{};
    for (int ring = 0; ring < ring_count; ++ring) {
        const float t = static_cast<float>(ring + 1) / static_cast<float>(std::max(1, ring_count));
        const float w = (1.0f - t * 0.62f) * (1.0f - t * 0.62f);
        ring_weights[static_cast<std::size_t>(ring)] = w;
        total_raw_weight += w * static_cast<float>(kCircleDirections) * kCircularBlurWeight;
    }

    std::array<float, dof_blur_chain::radial_blur_tuning::kMaxSamples> zoom_weights{};
    for (int i = 0; i < zoom_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(std::max(1, zoom_count));
        const float w = (1.0f - t) * (1.0f - t);
        zoom_weights[static_cast<std::size_t>(i)] = w;
        total_raw_weight += w * kZoomBlurWeight;
    }

    if (total_raw_weight <= 1.0e-6f) {
        restore_texture_state(src, src_state);
        render_diagnostics::set_render_target(renderer, previous_target);
        return false;
    }

    draw_weighted_scaled_sample(renderer,
                                src,
                                draw_w,
                                draw_h,
                                center,
                                1.0f,
                                1.0f / total_raw_weight);

    for (int ring = 0; ring < ring_count; ++ring) {
        const float ring_t = static_cast<float>(ring + 1) / static_cast<float>(std::max(1, ring_count));
        const float offset_radius = circular_radius_px * ring_t;
        const float weight =
            (ring_weights[static_cast<std::size_t>(ring)] * kCircularBlurWeight) / total_raw_weight;

        for (int dir = 0; dir < kCircleDirections; ++dir) {
            const float angle =
                (static_cast<float>(dir) / static_cast<float>(kCircleDirections)) * 6.283185307179586f;
            const float offset_x = std::cos(angle) * offset_radius;
            const float offset_y = std::sin(angle) * offset_radius;

            draw_weighted_offset_sample(renderer,
                                        src,
                                        draw_w,
                                        draw_h,
                                        offset_x,
                                        offset_y,
                                        weight);
        }
    }

    for (int i = 0; i < zoom_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(std::max(1, zoom_count));
        const float scale_delta = max_zoom_scale_delta * t;
        const float weight =
            (zoom_weights[static_cast<std::size_t>(i)] * kZoomBlurWeight) / total_raw_weight;

        draw_weighted_scaled_sample(renderer,
                                    src,
                                    draw_w,
                                    draw_h,
                                    center,
                                    1.0f + scale_delta,
                                    weight);
    }

    restore_texture_state(src, src_state);
    render_diagnostics::set_render_target(renderer, previous_target);

    return true;
}

bool apply_radial_only_lens_blur(SDL_Renderer* renderer,
                                 SDL_Texture* src,
                                 SDL_Texture* dst,
                                 SDL_Texture* scratch,
                                 int target_w,
                                 int target_h,
                                 float radial_radius_px,
                                 float quality_scale) {
    if (!renderer || !src || !dst || !scratch || target_w <= 0 || target_h <= 0 ||
        scratch == src || scratch == dst) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);
    const TextureStateSnapshot scratch_state = capture_texture_state(scratch);
    const TextureStateSnapshot dst_state = capture_texture_state(dst);

    const SDL_BlendMode sum_blend = make_sum_blend_mode();
    if (sum_blend == SDL_BLENDMODE_INVALID) {
        const bool copied = copy_texture_region(renderer, src, dst, nullptr, nullptr);

        restore_texture_state(src, src_state);
        restore_texture_state(scratch, scratch_state);
        restore_texture_state(dst, dst_state);
        render_diagnostics::set_render_target(renderer, previous_target);

        return copied;
    }

    const float clamped_quality =
        std::clamp(quality_scale, dof_blur_chain::radial_blur_tuning::kMinProcessQualityScale, 1.0f);

    const int process_w =
        std::clamp(static_cast<int>(std::lround(static_cast<float>(target_w) * clamped_quality)), 1, target_w);
    const int process_h =
        std::clamp(static_cast<int>(std::lround(static_cast<float>(target_h) * clamped_quality)), 1, target_h);

    const bool reduced_resolution = (process_w != target_w) || (process_h != target_h);
    const float process_radial_radius = std::max(0.0f, radial_radius_px) * clamped_quality;

    if (process_radial_radius <= kEffectEpsilon) {
        const bool copied = copy_texture_region(renderer, src, dst, nullptr, nullptr);

        restore_texture_state(src, src_state);
        restore_texture_state(scratch, scratch_state);
        restore_texture_state(dst, dst_state);
        render_diagnostics::set_render_target(renderer, previous_target);

        return copied;
    }

    SDL_Texture* current = src;

    if (reduced_resolution) {
        const SDL_FRect lowres_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };

        if (!copy_texture_region(renderer, src, scratch, nullptr, &lowres_rect)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            render_diagnostics::set_render_target(renderer, previous_target);
            return false;
        }

        current = scratch;
    }

    if (!apply_fast_radial_zoom_blur(renderer,
                                     current,
                                     dst,
                                     process_w,
                                     process_h,
                                     process_radial_radius,
                                     sum_blend)) {
        restore_texture_state(src, src_state);
        restore_texture_state(scratch, scratch_state);
        restore_texture_state(dst, dst_state);
        render_diagnostics::set_render_target(renderer, previous_target);
        return false;
    }

    bool copied_to_dst = true;

    if (reduced_resolution) {
        const SDL_FRect lowres_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };

        copied_to_dst = copy_texture_region(renderer, dst, scratch, &lowres_rect, &lowres_rect);
        if (copied_to_dst) {
            copied_to_dst = copy_texture_region(renderer, scratch, dst, &lowres_rect, nullptr);
        }
    }

    restore_texture_state(src, src_state);
    restore_texture_state(scratch, scratch_state);
    restore_texture_state(dst, dst_state);
    render_diagnostics::set_render_target(renderer, previous_target);

    return copied_to_dst;
}

float compute_radial_quality_scale(int width,
                                   int height,
                                   float radial_blur_px,
                                   float normalized_layer_distance,
                                   bool foreground_layer) {
    if (foreground_layer) {
        return 1.0f;
    }

    const float safe_radius = sanitized_non_negative(radial_blur_px);
    const int min_dim = std::max(1, std::min(width, height));

    if (safe_radius <= 2.0f || min_dim <= 360) {
        return 1.0f;
    }

    float quality = 1.0f;

    if (safe_radius <= 5.0f) {
        quality = 0.82f;
    } else if (safe_radius <= 10.0f) {
        quality = 0.66f;
    } else if (safe_radius <= 18.0f) {
        quality = 0.50f;
    } else if (safe_radius <= 32.0f) {
        quality = 0.36f;
    } else {
        quality = dof_blur_chain::radial_blur_tuning::kMinProcessQualityScale;
    }

    const float distance_t = safe_unit(normalized_layer_distance);
    const float distance_multiplier =
        1.0f - distance_t * (1.0f - dof_blur_chain::radial_blur_tuning::kFarLayerQualityMultiplier);

    quality *= distance_multiplier;

    return std::clamp(quality, dof_blur_chain::radial_blur_tuning::kMinProcessQualityScale, 1.0f);
}

} // namespace

namespace dof_blur_chain {

bool enabled(bool depth_of_field_enabled, float /*blur_px*/, float radial_blur_px) {
    const bool radial_enabled =
        depth_of_field_enabled &&
        sanitized_non_negative(radial_blur_px) > kEffectEpsilon;

    return radial_enabled || atmospheric_dust_tuning::kEnabled;
}

Renderer::Renderer(SDL_Renderer* renderer)
    : renderer_(renderer) {}

Renderer::~Renderer() {
    destroy_targets();
}

void Renderer::set_renderer(SDL_Renderer* renderer) {
    if (renderer_ == renderer) {
        return;
    }

    destroy_targets();
    renderer_ = renderer;
}

void Renderer::set_output_dimensions(int width, int height) {
    const int safe_width = std::max(1, width);
    const int safe_height = std::max(1, height);

    if (safe_width == width_ && safe_height == height_) {
        return;
    }

    width_ = safe_width;
    height_ = safe_height;

    destroy_targets();
}

void Renderer::set_dust_frames(const std::vector<SDL_Texture*>& dust_frames) {
    dust_frames_.clear();
    dust_frames_.reserve(dust_frames.size());

    for (SDL_Texture* texture : dust_frames) {
        if (texture) {
            dust_frames_.push_back(texture);
        }
    }
}

void Renderer::destroy_targets() {
    render_diagnostics::destroy_texture(background_mid_);
    render_diagnostics::destroy_texture(foreground_mid_);
    render_diagnostics::destroy_texture(foreground_layer_);
    render_diagnostics::destroy_texture(chain_temp_);
    render_diagnostics::destroy_texture(blur_work_);
    render_diagnostics::destroy_texture(dust_work_);
}

bool Renderer::ensure_target(SDL_Texture*& texture, const char* label) {
    if (!renderer_ || width_ <= 0 || height_ <= 0) {
        return false;
    }

    if (texture) {
        float w = 0.0f;
        float h = 0.0f;

        if (SDL_GetTextureSize(texture, &w, &h) &&
            static_cast<int>(std::lround(w)) == width_ &&
            static_cast<int>(std::lround(h)) == height_) {
            return true;
        }

        render_diagnostics::destroy_texture(texture);
    }

    texture = render_diagnostics::create_texture(
        renderer_,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET,
        width_,
        height_);

    if (!texture) {
        return false;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);

    (void)label;
    return true;
}

bool Renderer::ensure_targets() {
    return ensure_target(background_mid_, "dof_background_mid") &&
           ensure_target(foreground_mid_, "dof_foreground_mid") &&
           ensure_target(foreground_layer_, "dof_foreground_layer") &&
           ensure_target(chain_temp_, "dof_chain_temp") &&
           ensure_target(blur_work_, "dof_blur_work") &&
           ensure_target(dust_work_, "dof_dust_work");
}

void Renderer::clear_target(SDL_Texture* texture) const {
    clear_texture_target(renderer_, texture);
}

bool Renderer::copy_texture(SDL_Texture* src, SDL_Texture* dst) const {
    return copy_texture_region(renderer_, src, dst, nullptr, nullptr);
}

bool Renderer::composite_texture_over(SDL_Texture* src, SDL_Texture* dst) const {
    if (!renderer_ || !src || !dst) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    const TextureStateSnapshot src_state = capture_texture_state(src);

    render_diagnostics::set_render_target(renderer_, dst);

    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const bool ok = render_diagnostics::render_texture(renderer_, src, nullptr, nullptr);

    restore_texture_state(src, src_state);
    render_diagnostics::set_render_target(renderer_, previous_target);

    return ok;
}

bool Renderer::blur_step(SDL_Texture* src,
                         SDL_Texture* dst,
                         SDL_Texture* blur_work,
                         float /*blur_px*/,
                         SDL_FPoint /*optical_center*/,
                         float radial_blur_px,
                         float quality_scale) const {
    if (!src || !dst || !blur_work) {
        return false;
    }

    const float safe_radial_blur_px = sanitized_non_negative(radial_blur_px);
    if (safe_radial_blur_px <= kEffectEpsilon) {
        return copy_texture(src, dst);
    }

    return apply_radial_only_lens_blur(renderer_,
                                       src,
                                       dst,
                                       blur_work,
                                       width_,
                                       height_,
                                       safe_radial_blur_px,
                                       quality_scale);
}

CompositeResult Renderer::compose(const std::vector<LayerTexture>& layers,
                                  SDL_Texture* background_seed,
                                  bool depth_of_field_enabled,
                                  float /*blur_px*/,
                                  float radial_blur_px,
                                  SDL_FPoint optical_center,
                                  int focus_depth_layer,
                                  float camera_zoom_percent,
                                  float time_seconds,
                                  DustAnchor dust_anchor) {
    (void)optical_center;
    (void)camera_zoom_percent;

    CompositeResult result{};

    if (!renderer_ || layers.empty()) {
        return result;
    }

    if (!ensure_targets()) {
        return result;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);

    auto restore_and_return = [&](CompositeResult out) {
        render_diagnostics::set_render_target(renderer_, previous_target);
        return out;
    };

    const float safe_radial_blur_px = sanitized_non_negative(radial_blur_px);
    const bool effect_enabled = enabled(depth_of_field_enabled, 0.0f, safe_radial_blur_px);

    scratch_background_layers_.clear();
    scratch_foreground_layers_.clear();

    int max_layer_distance = 0;

    for (const LayerTexture& layer : layers) {
        if (!layer.texture) {
            continue;
        }

        max_layer_distance = std::max(max_layer_distance, std::abs(layer.depth_layer - focus_depth_layer));

        if (layer.depth_layer > focus_depth_layer) {
            scratch_background_layers_.push_back(layer);
        } else if (layer.depth_layer < focus_depth_layer) {
            scratch_foreground_layers_.push_back(layer);
        }
    }

    std::sort(scratch_background_layers_.begin(),
              scratch_background_layers_.end(),
              [](const LayerTexture& a, const LayerTexture& b) {
                  return a.depth_layer > b.depth_layer;
              });

    std::sort(scratch_foreground_layers_.begin(),
              scratch_foreground_layers_.end(),
              [](const LayerTexture& a, const LayerTexture& b) {
                  return a.depth_layer < b.depth_layer;
              });

    const float inv_max_layer_distance =
        max_layer_distance > 0 ? 1.0f / static_cast<float>(max_layer_distance) : 0.0f;

    clear_target(background_mid_);
    clear_target(foreground_mid_);

    bool background_has_content = false;
    bool foreground_has_content = false;

    auto resolve_layer_source = [&](const LayerTexture& layer, SDL_Texture*& out_texture) -> bool {
        out_texture = layer.texture;

        if (!out_texture) {
            return false;
        }

        if (std::abs(layer.warp_px) <= kEffectEpsilon && layer.tint_strength <= kEffectEpsilon) {
            return true;
        }

        if (!prepare_damage_pulse_texture(renderer_,
                                          layer.texture,
                                          chain_temp_,
                                          width_,
                                          height_,
                                          layer.warp_px,
                                          layer.tint_strength,
                                          layer.phase)) {
            return false;
        }

        out_texture = chain_temp_;
        return true;
    };

    auto add_dust_to_layer = [&](SDL_Texture* source,
                                 SDL_Texture*& out_source,
                                 int layer_id,
                                 bool foreground_layer,
                                 bool background_seed) -> bool {
        if (!effect_enabled) {
            out_source = source;
            return true;
        }

        if (!add_layer_atmospheric_dust(renderer_,
                                        source,
                                        dust_work_,
                                        dust_frames_,
                                        width_,
                                        height_,
                                        layer_id,
                                        focus_depth_layer,
                                        std::max(1, max_layer_distance),
                                        time_seconds,
                                        foreground_layer,
                                        background_seed,
                                        dust_anchor)) {
            return false;
        }

        out_source = dust_work_;
        return true;
    };

    if (background_seed) {
        const int seed_distance = std::max(4, max_layer_distance);
        const int seed_layer = focus_depth_layer + seed_distance;

        SDL_Texture* seed_source = nullptr;

        if (!add_dust_to_layer(background_seed, seed_source, seed_layer, false, true)) {
            return restore_and_return(CompositeResult{});
        }

        const float seed_radial = depth_of_field_enabled ? safe_radial_blur_px * 0.70f : 0.0f;

        if (seed_radial > kEffectEpsilon) {
            const float background_quality = std::clamp(
                compute_radial_quality_scale(width_, height_, seed_radial, 1.0f, false) *
                    radial_blur_tuning::kBackgroundSeedQualityMultiplier,
                radial_blur_tuning::kMinProcessQualityScale,
                1.0f);

            if (!blur_step(seed_source,
                           background_mid_,
                           blur_work_,
                           0.0f,
                           screen_center_for_target(width_, height_),
                           seed_radial,
                           background_quality)) {
                return restore_and_return(CompositeResult{});
            }

            ++result.blur_pass_count;
        } else if (!copy_texture(seed_source, background_mid_)) {
            return restore_and_return(CompositeResult{});
        }

        background_has_content = true;
    }

    auto process_and_composite_layer =
        [&](const LayerTexture& layer,
            SDL_Texture* composite_target,
            bool foreground_layer) -> bool {
            SDL_Texture* resolved_source = nullptr;

            if (!resolve_layer_source(layer, resolved_source)) {
                return false;
            }

            SDL_Texture* layer_source = nullptr;

            if (!add_dust_to_layer(resolved_source,
                                   layer_source,
                                   layer.depth_layer,
                                   foreground_layer,
                                   false)) {
                return false;
            }

            const int layer_distance = std::abs(layer.depth_layer - focus_depth_layer);
            const float layer_distance_t =
                std::clamp(static_cast<float>(layer_distance) * inv_max_layer_distance, 0.0f, 1.0f);

            const float layer_strength = std::clamp(layer.blur_strength, 0.0f, 1.0f);
            const float depth_t = depth_curve(layer_distance, layer_distance_t);

            const float layer_radial =
                depth_of_field_enabled
                    ? safe_radial_blur_px * layer_strength * depth_t
                    : 0.0f;

            SDL_Texture* layer_output = layer_source;

            if (layer_radial > kEffectEpsilon) {
                const float layer_quality =
                    compute_radial_quality_scale(width_,
                                                 height_,
                                                 layer_radial,
                                                 layer_distance_t,
                                                 foreground_layer);

                if (!blur_step(layer_source,
                               foreground_layer_,
                               blur_work_,
                               0.0f,
                               screen_center_for_target(width_, height_),
                               layer_radial,
                               layer_quality)) {
                    return false;
                }

                ++result.blur_pass_count;
                layer_output = foreground_layer_;
            }

            return composite_texture_over(layer_output, composite_target);
        };

    for (const LayerTexture& layer : scratch_background_layers_) {
        if (!process_and_composite_layer(layer, background_mid_, false)) {
            return restore_and_return(CompositeResult{});
        }

        background_has_content = true;
    }

    for (const LayerTexture& layer : layers) {
        if (layer.depth_layer != focus_depth_layer || !layer.texture) {
            continue;
        }

        SDL_Texture* resolved_source = nullptr;

        if (!resolve_layer_source(layer, resolved_source)) {
            return restore_and_return(CompositeResult{});
        }

        SDL_Texture* layer_source = nullptr;

        if (!add_dust_to_layer(resolved_source,
                               layer_source,
                               layer.depth_layer,
                               false,
                               false)) {
            return restore_and_return(CompositeResult{});
        }

        constexpr float kFocusLayerRadialMultiplier = 0.025f;

        const float focus_radial =
            depth_of_field_enabled
                ? safe_radial_blur_px *
                      std::clamp(layer.blur_strength, 0.0f, 1.0f) *
                      kFocusLayerRadialMultiplier
                : 0.0f;

        SDL_Texture* output = layer_source;

        if (focus_radial > kEffectEpsilon) {
            if (!blur_step(layer_source,
                           foreground_layer_,
                           blur_work_,
                           0.0f,
                           screen_center_for_target(width_, height_),
                           focus_radial,
                           1.0f)) {
                return restore_and_return(CompositeResult{});
            }

            ++result.blur_pass_count;
            output = foreground_layer_;
        }

        if (!composite_texture_over(output, background_mid_)) {
            return restore_and_return(CompositeResult{});
        }

        background_has_content = true;
    }

    for (const LayerTexture& layer : scratch_foreground_layers_) {
        if (!process_and_composite_layer(layer, foreground_mid_, true)) {
            return restore_and_return(CompositeResult{});
        }

        foreground_has_content = true;
    }

    result.valid = background_has_content || foreground_has_content;
    result.background_mid = background_has_content ? background_mid_ : nullptr;
    result.foreground_mid = foreground_has_content ? foreground_mid_ : nullptr;

    return restore_and_return(result);
}

} // namespace dof_blur_chain