#include "rendering/render/dof_blur_chain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "rendering/render/render_diagnostics.hpp"

namespace {

constexpr float kEffectEpsilon = 1.0e-4f;

// Fog is intentionally tiny. 0.05f = 5% alpha, not 50%.
constexpr bool kLayerFogEnabled = true;
constexpr float kLayerFogMaxOpacity = 0.050f;
constexpr float kLayerFogFocusOpacity = 0.006f;
constexpr float kLayerFogMinDepthOpacity = 0.010f;
constexpr float kLayerFogTopAboveHorizonRatio = 0.035f;
constexpr float kLayerFogHorizonRatio = 0.385f;
constexpr float kLayerFogBottomRatio = 0.965f;
constexpr float kLayerFogBackgroundBaseRatio = 0.790f;
constexpr float kLayerFogGrey = 0.66f;
constexpr int kLayerFogColumns = 12;

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

float lerp_float(float a, float b, float t) {
    return a + (b - a) * safe_unit(t);
}

float smoothstep01(float value) {
    const float t = safe_unit(value);
    return t * t * (3.0f - 2.0f * t);
}

float absolute_depth_effect_t(int layer_distance) {
    if (layer_distance <= 0) {
        return 0.0f;
    }

    // Adjacent layers should barely change. Deeper layers ramp smoothly.
    const float t = std::clamp((static_cast<float>(layer_distance) - 0.35f) / 4.75f, 0.0f, 1.0f);
    const float s = smoothstep01(t);
    return std::pow(s, 2.10f);
}

float normalized_depth_effect_t(float normalized_distance) {
    const float s = smoothstep01(normalized_distance);
    return std::pow(s, dof_blur_chain::layer_effect_tuning::kDepthRampPower);
}

float lens_realism_depth_t(int layer_distance, float normalized_distance) {
    const float a = absolute_depth_effect_t(layer_distance);
    const float n = normalized_depth_effect_t(normalized_distance);
    return std::clamp(a * 0.78f + n * 0.22f, 0.0f, 1.0f);
}

float dof_realism_depth_t(int layer_distance, float normalized_distance) {
    const float a = absolute_depth_effect_t(layer_distance);
    const float n = normalized_depth_effect_t(normalized_distance);

    // Softer than the old curve so layer-to-layer movement feels smooth.
    return std::clamp(a * 0.86f + n * 0.14f, 0.0f, 1.0f);
}

float fog_depth_t(int layer_distance, float normalized_distance) {
    if (layer_distance <= 0) {
        return 0.0f;
    }

    const float a = absolute_depth_effect_t(layer_distance);
    const float n = normalized_depth_effect_t(normalized_distance);
    return std::clamp(a * 0.72f + n * 0.28f, 0.0f, 1.0f);
}

SDL_FPoint render_target_center(int width, int height) {
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

void draw_scaled_sample(SDL_Renderer* renderer,
                        SDL_Texture* texture,
                        int draw_w,
                        int draw_h,
                        const SDL_FPoint& center,
                        float scale,
                        Uint8 alpha) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || alpha == 0) {
        return;
    }

    const float safe_scale = std::max(0.001f, scale);

    SDL_SetTextureAlphaMod(texture, alpha);

    const float src_w = static_cast<float>(draw_w);
    const float src_h = static_cast<float>(draw_h);
    const float scaled_w = src_w * safe_scale;
    const float scaled_h = src_h * safe_scale;

    const SDL_FRect src_rect{0.0f, 0.0f, src_w, src_h};
    const SDL_FRect dst_rect{
        center.x - center.x * safe_scale,
        center.y - center.y * safe_scale,
        scaled_w,
        scaled_h
    };

    (void)render_diagnostics::render_texture(renderer, texture, &src_rect, &dst_rect);
}

void draw_weighted_scaled_sample(SDL_Renderer* renderer,
                                 SDL_Texture* texture,
                                 int draw_w,
                                 int draw_h,
                                 const SDL_FPoint& center,
                                 float scale,
                                 float weight) {
    const int alpha = std::clamp(static_cast<int>(std::lround(std::max(0.0f, weight) * 255.0f)), 0, 255);
    draw_scaled_sample(renderer, texture, draw_w, draw_h, center, scale, static_cast<Uint8>(alpha));
}

std::uint32_t hash_layer_column(int layer_id, int column, int salt) {
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::uint32_t v) {
        h ^= v;
        h *= 16777619u;
    };
    mix(static_cast<std::uint32_t>(layer_id * 73856093));
    mix(static_cast<std::uint32_t>(column * 19349663));
    mix(static_cast<std::uint32_t>(salt * 83492791));
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return h;
}

float jitter_unit(int layer_id, int column, int salt) {
    const std::uint32_t h = hash_layer_column(layer_id, column, salt);
    const float unit = static_cast<float>(h & 0xFFFFu) / 65535.0f;
    return unit * 2.0f - 1.0f;
}

void emit_fog_quad(std::vector<SDL_Vertex>& vertices,
                   std::vector<int>& indices,
                   float x0,
                   float x1,
                   float y0,
                   float y1,
                   float alpha0,
                   float alpha1) {
    if (x1 <= x0 || y1 <= y0 || alpha0 <= 0.0f && alpha1 <= 0.0f) {
        return;
    }

    const int base = static_cast<int>(vertices.size());

    const SDL_FColor top_color{
        kLayerFogGrey,
        kLayerFogGrey,
        kLayerFogGrey,
        std::clamp(alpha0, 0.0f, 1.0f)
    };

    const SDL_FColor bottom_color{
        kLayerFogGrey,
        kLayerFogGrey,
        kLayerFogGrey,
        std::clamp(alpha1, 0.0f, 1.0f)
    };

    SDL_Vertex tl{};
    tl.position = SDL_FPoint{x0, y0};
    tl.color = top_color;

    SDL_Vertex tr{};
    tr.position = SDL_FPoint{x1, y0};
    tr.color = top_color;

    SDL_Vertex br{};
    br.position = SDL_FPoint{x1, y1};
    br.color = bottom_color;

    SDL_Vertex bl{};
    bl.position = SDL_FPoint{x0, y1};
    bl.color = bottom_color;

    vertices.push_back(tl);
    vertices.push_back(tr);
    vertices.push_back(br);
    vertices.push_back(bl);

    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

bool add_layer_depth_fog(SDL_Renderer* renderer,
                         SDL_Texture* src,
                         SDL_Texture* dst,
                         int width,
                         int height,
                         int depth_layer,
                         int focus_depth_layer,
                         int max_layer_distance) {
    if (!renderer || !src || !dst || width <= 0 || height <= 0 || src == dst) {
        return false;
    }

    if (!copy_texture_region(renderer, src, dst, nullptr, nullptr)) {
        return false;
    }

    if (!kLayerFogEnabled) {
        return true;
    }

    const int layer_distance = std::abs(depth_layer - focus_depth_layer);
    const bool foreground_layer = depth_layer < focus_depth_layer;
    const bool background_layer = depth_layer > focus_depth_layer;
    const float inv_max_distance =
        max_layer_distance > 0 ? 1.0f / static_cast<float>(max_layer_distance) : 0.0f;
    const float normalized_distance =
        std::clamp(static_cast<float>(layer_distance) * inv_max_distance, 0.0f, 1.0f);

    const float depth_t = fog_depth_t(layer_distance, normalized_distance);

    float peak_alpha = kLayerFogFocusOpacity;
    if (layer_distance > 0) {
        peak_alpha = kLayerFogMinDepthOpacity +
            (kLayerFogMaxOpacity - kLayerFogMinDepthOpacity) * depth_t;
    }
    peak_alpha = std::clamp(peak_alpha, 0.0f, kLayerFogMaxOpacity);

    if (peak_alpha <= 1.0f / 255.0f) {
        return true;
    }

    const float h = static_cast<float>(height);
    const float w = static_cast<float>(width);

    const float horizon_y = h * kLayerFogHorizonRatio;
    const float top_y_base = std::max(0.0f, horizon_y - h * kLayerFogTopAboveHorizonRatio);

    float depth_line_y = h * 0.64f;
    float bottom_y_base = h * kLayerFogBottomRatio;

    if (background_layer) {
        // Farther background layers sit closer to the horizon.
        depth_line_y = lerp_float(h * 0.61f, horizon_y + h * 0.055f, depth_t);
        bottom_y_base = lerp_float(h * kLayerFogBackgroundBaseRatio, h * 0.60f, depth_t);
    } else if (foreground_layer) {
        // Near foreground fog reaches lower on screen.
        depth_line_y = lerp_float(h * 0.66f, h * 0.84f, depth_t);
        bottom_y_base = h * kLayerFogBottomRatio;
    } else {
        // Focus layer gets only a very faint lens haze around its depth band.
        depth_line_y = h * 0.64f;
        bottom_y_base = h * 0.83f;
    }

    depth_line_y = std::clamp(depth_line_y, top_y_base + 2.0f, h - 2.0f);
    bottom_y_base = std::clamp(bottom_y_base, depth_line_y + 2.0f, h + 64.0f);

    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(static_cast<std::size_t>(kLayerFogColumns * 8));
    indices.reserve(static_cast<std::size_t>(kLayerFogColumns * 12));

    const float column_w = w / static_cast<float>(kLayerFogColumns);
    const float edge_jitter_px = std::clamp(h * 0.010f, 2.0f, 14.0f);
    const float depth_jitter_px = std::clamp(h * 0.0075f, 1.5f, 9.0f);

    for (int column = 0; column < kLayerFogColumns; ++column) {
        const float x0 = static_cast<float>(column) * column_w - 1.0f;
        const float x1 = (column == kLayerFogColumns - 1)
            ? w + 1.0f
            : static_cast<float>(column + 1) * column_w + 1.0f;

        const float top_y =
            std::clamp(top_y_base + jitter_unit(depth_layer, column, 1) * edge_jitter_px,
                       0.0f,
                       depth_line_y - 2.0f);

        const float local_depth_y =
            std::clamp(depth_line_y + jitter_unit(depth_layer, column, 2) * depth_jitter_px,
                       top_y + 2.0f,
                       bottom_y_base - 2.0f);

        const float bottom_y =
            std::clamp(bottom_y_base + jitter_unit(depth_layer, column, 3) * edge_jitter_px,
                       local_depth_y + 2.0f,
                       h + 72.0f);

        const float column_alpha_scale =
            std::clamp(0.86f + jitter_unit(depth_layer, column, 4) * 0.14f, 0.72f, 1.0f);

        const float local_peak_alpha = peak_alpha * column_alpha_scale;

        // Fade upward from the depth line toward just above the horizon.
        emit_fog_quad(vertices,
                      indices,
                      x0,
                      x1,
                      top_y,
                      local_depth_y,
                      0.0f,
                      local_peak_alpha);

        // Fade downward from the depth line toward the layer base.
        emit_fog_quad(vertices,
                      indices,
                      x0,
                      x1,
                      local_depth_y,
                      bottom_y,
                      local_peak_alpha,
                      local_peak_alpha * 0.10f);
    }

    if (vertices.empty() || indices.empty()) {
        return true;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    render_diagnostics::set_render_target(renderer, dst);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const bool ok = SDL_RenderGeometry(renderer,
                                       nullptr,
                                       vertices.data(),
                                       static_cast<int>(vertices.size()),
                                       indices.data(),
                                       static_cast<int>(indices.size()));

    render_diagnostics::set_render_target(renderer, previous_target);
    return ok;
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

    const float safe_warp = std::clamp(std::abs(warp_px), 0.0f, dof_blur_chain::damage_pulse_tuning::kMaxWarpPx);
    const float safe_tint = std::clamp(tint_strength, 0.0f, dof_blur_chain::damage_pulse_tuning::kMaxTintStrength);

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

int radial_sample_count(float radial_radius_px) {
    const float safe_radius = sanitized_non_negative(radial_radius_px);
    if (safe_radius <= kEffectEpsilon) {
        return 0;
    }

    return std::clamp(
        dof_blur_chain::layer_effect_tuning::kMinRadialSamples +
            static_cast<int>(std::ceil(std::sqrt(safe_radius) * dof_blur_chain::layer_effect_tuning::kSamplesPerSqrtRadius)),
        dof_blur_chain::layer_effect_tuning::kMinRadialSamples,
        dof_blur_chain::layer_effect_tuning::kMaxRadialSamples);
}

float compute_background_quality_scale(int width,
                                       int height,
                                       float radial_blur_px,
                                       float normalized_distance) {
    const int min_dim = std::max(1, std::min(width, height));
    const float safe_radius = sanitized_non_negative(radial_blur_px);

    if (min_dim <= 360 || safe_radius <= 2.0f) {
        return 1.0f;
    }

    float quality = 1.0f;

    if (safe_radius <= 5.0f) {
        quality = 0.86f;
    } else if (safe_radius <= 10.0f) {
        quality = 0.70f;
    } else if (safe_radius <= 18.0f) {
        quality = 0.54f;
    } else if (safe_radius <= 32.0f) {
        quality = 0.40f;
    } else {
        quality = dof_blur_chain::layer_effect_tuning::kMinProcessQualityScale;
    }

    const float distance_t = safe_unit(normalized_distance);
    const float distance_multiplier =
        1.0f - distance_t * (1.0f - dof_blur_chain::layer_effect_tuning::kBackgroundFarQualityMultiplier);

    quality *= distance_multiplier;

    return std::clamp(quality, dof_blur_chain::layer_effect_tuning::kMinProcessQualityScale, 1.0f);
}

float compute_chromatic_px(int layer_distance,
                           float normalized_distance,
                           bool foreground_layer,
                           bool background_seed) {
    if (!dof_blur_chain::layer_effect_tuning::kChromaticAberrationEnabled) {
        return 0.0f;
    }

    const float depth_amount = lens_realism_depth_t(layer_distance, normalized_distance);

    float multiplier = foreground_layer
        ? dof_blur_chain::layer_effect_tuning::kChromaticForegroundMultiplier
        : dof_blur_chain::layer_effect_tuning::kChromaticBackgroundMultiplier;

    if (background_seed) {
        multiplier *= dof_blur_chain::layer_effect_tuning::kChromaticBackgroundSeedMultiplier;
    }

    // Kept restrained so it does not wash the whole image purple.
    const float px =
        (0.25f + dof_blur_chain::layer_effect_tuning::kChromaticBasePx * 0.42f +
         dof_blur_chain::layer_effect_tuning::kChromaticMaxPx * 0.44f * depth_amount) *
        multiplier;

    return std::clamp(px, 0.0f, 5.25f);
}

bool apply_radial_zoom_blur(SDL_Renderer* renderer,
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

    const SDL_FPoint center = render_target_center(draw_w, draw_h);

    SDL_SetTextureBlendMode(src, sum_blend);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const float max_dim = static_cast<float>(std::max(draw_w, draw_h));
    const float normalized_radius = std::clamp(radial_radius_px / std::max(1.0f, max_dim), 0.0f, 1.0f);
    const int sample_count = radial_sample_count(radial_radius_px);

    const float max_blur_scale_delta = std::min(
        dof_blur_chain::layer_effect_tuning::kMaxRadialScaleDelta,
        normalized_radius * dof_blur_chain::layer_effect_tuning::kRadialScaleMultiplier);

    float total_raw_weight = 1.0f;
    std::array<float, dof_blur_chain::layer_effect_tuning::kMaxRadialSamples> weights{};

    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float w = (1.0f - t) * (1.0f - t);
        weights[static_cast<std::size_t>(i)] = w;
        total_raw_weight += w;
    }

    if (total_raw_weight <= 1.0e-6f) {
        restore_texture_state(src, src_state);
        render_diagnostics::set_render_target(renderer, previous_target);
        return false;
    }

    // Full coverage base sample. Only outward samples, so no black corner creep.
    draw_weighted_scaled_sample(renderer,
                                src,
                                draw_w,
                                draw_h,
                                center,
                                1.0f,
                                1.0f / total_raw_weight);

    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float scale_delta = max_blur_scale_delta * t;
        const float weight = weights[static_cast<std::size_t>(i)] / total_raw_weight;

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

bool apply_chromatic_aberration(SDL_Renderer* renderer,
                                SDL_Texture* src,
                                SDL_Texture* dst,
                                int width,
                                int height,
                                float chromatic_px) {
    if (!renderer || !src || !dst || width <= 0 || height <= 0 || src == dst) {
        return false;
    }

    if (chromatic_px <= kEffectEpsilon) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);

    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);

    const SDL_FPoint center = render_target_center(width, height);
    const float max_dim = static_cast<float>(std::max(width, height));
    const float scale_delta = std::clamp(chromatic_px / std::max(1.0f, max_dim), 0.0f, 0.014f);

    const float requested_fringe = dof_blur_chain::layer_effect_tuning::kChromaticFringeOpacity;
    const float fringe_opacity = std::clamp(requested_fringe * 0.30f, 0.035f, 0.155f);
    const Uint8 fringe_alpha = static_cast<Uint8>(std::clamp(
        static_cast<int>(std::lround(fringe_opacity * 255.0f)),
        0,
        255));

    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);

    // Real image first. Aberration is only a fringe, never the main color.
    SDL_SetTextureColorMod(src, 255, 255, 255);
    draw_scaled_sample(renderer, src, width, height, center, 1.0f, 255);

    SDL_SetTextureColorMod(src, 255, 48, 28);
    draw_scaled_sample(renderer, src, width, height, center, 1.0f + scale_delta * 1.45f, fringe_alpha);

    SDL_SetTextureColorMod(src, 40, 210, 255);
    draw_scaled_sample(renderer, src, width, height, center, std::max(0.988f, 1.0f - scale_delta * 0.80f), fringe_alpha);

    restore_texture_state(src, src_state);
    render_diagnostics::set_render_target(renderer, previous_target);
    return true;
}

bool apply_layer_effect_pass(SDL_Renderer* renderer,
                             SDL_Texture* src,
                             SDL_Texture* dst,
                             SDL_Texture* work,
                             SDL_Texture* chromatic_work,
                             int width,
                             int height,
                             float radial_blur_px,
                             float quality_scale,
                             float chromatic_px) {
    if (!renderer || !src || !dst || !work || !chromatic_work || width <= 0 || height <= 0) {
        return false;
    }

    const SDL_BlendMode sum_blend = make_sum_blend_mode();
    if (sum_blend == SDL_BLENDMODE_INVALID) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    const float safe_quality = std::clamp(
        quality_scale,
        dof_blur_chain::layer_effect_tuning::kMinProcessQualityScale,
        1.0f);

    const int process_w = std::clamp(static_cast<int>(std::lround(static_cast<float>(width) * safe_quality)), 1, width);
    const int process_h = std::clamp(static_cast<int>(std::lround(static_cast<float>(height) * safe_quality)), 1, height);
    const bool reduced_resolution = process_w != width || process_h != height;

    const float process_radial = sanitized_non_negative(radial_blur_px) * safe_quality;
    const float process_chromatic = sanitized_non_negative(chromatic_px) * safe_quality;

    SDL_Texture* current = src;

    if (reduced_resolution) {
        const SDL_FRect low_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };

        if (!copy_texture_region(renderer, src, work, nullptr, &low_rect)) {
            return false;
        }

        current = work;
    }

    if (process_radial > kEffectEpsilon) {
        SDL_Texture* radial_dst = (current == work) ? chromatic_work : work;

        if (!apply_radial_zoom_blur(renderer,
                                    current,
                                    radial_dst,
                                    process_w,
                                    process_h,
                                    process_radial,
                                    sum_blend)) {
            return false;
        }

        current = radial_dst;
    }

    if (process_chromatic > kEffectEpsilon) {
        SDL_Texture* chroma_dst = (current == work) ? chromatic_work : work;

        if (!apply_chromatic_aberration(renderer,
                                        current,
                                        chroma_dst,
                                        process_w,
                                        process_h,
                                        process_chromatic)) {
            return false;
        }

        current = chroma_dst;
    }

    if (reduced_resolution) {
        const SDL_FRect low_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };

        return copy_texture_region(renderer, current, dst, &low_rect, nullptr);
    }

    if (current != dst) {
        return copy_texture_region(renderer, current, dst, nullptr, nullptr);
    }

    return true;
}

} // namespace

namespace dof_blur_chain {

bool enabled(bool depth_of_field_enabled, float /*blur_px*/, float /*radial_blur_px*/) {
    // If depth-of-field mode is active, this compositor now also owns layer fog.
    return depth_of_field_enabled;
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

void Renderer::destroy_targets() {
    render_diagnostics::destroy_texture(background_mid_);
    render_diagnostics::destroy_texture(foreground_mid_);
    render_diagnostics::destroy_texture(layer_effect_target_);
    render_diagnostics::destroy_texture(chain_temp_);
    render_diagnostics::destroy_texture(blur_work_);
    render_diagnostics::destroy_texture(chromatic_work_);
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
           ensure_target(layer_effect_target_, "dof_layer_effect_target") &&
           ensure_target(chain_temp_, "dof_chain_temp") &&
           ensure_target(blur_work_, "dof_blur_work") &&
           ensure_target(chromatic_work_, "dof_chromatic_work");
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

bool Renderer::process_layer_effects(SDL_Texture* src,
                                     SDL_Texture* dst,
                                     SDL_Texture* work,
                                     SDL_Texture* chromatic_work,
                                     float radial_blur_px,
                                     float quality_scale,
                                     float /*lens_scale_delta*/,
                                     float chromatic_px,
                                     bool /*foreground_layer*/) const {
    if (!src || !dst || !work || !chromatic_work) {
        return false;
    }

    if (radial_blur_px <= kEffectEpsilon &&
        chromatic_px <= kEffectEpsilon) {
        return copy_texture(src, dst);
    }

    return apply_layer_effect_pass(renderer_,
                                   src,
                                   dst,
                                   work,
                                   chromatic_work,
                                   width_,
                                   height_,
                                   radial_blur_px,
                                   quality_scale,
                                   chromatic_px);
}

CompositeResult Renderer::compose(const std::vector<LayerTexture>& layers,
                                  SDL_Texture* background_seed,
                                  bool depth_of_field_enabled,
                                  float /*blur_px*/,
                                  float radial_blur_px,
                                  SDL_FPoint optical_center,
                                  int focus_depth_layer,
                                  float camera_zoom_percent) {
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

    auto fail = [&]() {
        render_diagnostics::set_render_target(renderer_, previous_target);
        return CompositeResult{};
    };

    auto finish = [&](CompositeResult out) {
        render_diagnostics::set_render_target(renderer_, previous_target);
        return out;
    };

    const float base_radial_px = sanitized_non_negative(radial_blur_px);
    const bool dof_active = depth_of_field_enabled;

    scratch_background_layers_.clear();
    scratch_foreground_layers_.clear();

    int max_layer_distance = 0;

    for (const LayerTexture& layer : layers) {
        if (!layer.texture) {
            continue;
        }

        const int distance = std::abs(layer.depth_layer - focus_depth_layer);
        max_layer_distance = std::max(max_layer_distance, distance);

        if (layer.depth_layer > focus_depth_layer) {
            scratch_background_layers_.push_back(layer);
        } else if (layer.depth_layer < focus_depth_layer) {
            scratch_foreground_layers_.push_back(layer);
        }
    }

    const float inv_max_distance =
        max_layer_distance > 0 ? 1.0f / static_cast<float>(max_layer_distance) : 0.0f;

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

    auto process_and_composite_layer =
        [&](const LayerTexture& layer,
            SDL_Texture* target,
            bool foreground_layer) -> bool {
            SDL_Texture* source = nullptr;

            if (!resolve_layer_source(layer, source)) {
                return false;
            }

            const int layer_distance = std::abs(layer.depth_layer - focus_depth_layer);
            const float normalized_distance =
                std::clamp(static_cast<float>(layer_distance) * inv_max_distance, 0.0f, 1.0f);

            SDL_Texture* fogged_source = chromatic_work_;
            if (!add_layer_depth_fog(renderer_,
                                     source,
                                     fogged_source,
                                     width_,
                                     height_,
                                     layer.depth_layer,
                                     focus_depth_layer,
                                     max_layer_distance)) {
                return false;
            }

            const float dof_depth_t = dof_realism_depth_t(layer_distance, normalized_distance);
            const float lens_depth_t = lens_realism_depth_t(layer_distance, normalized_distance);

            const float radial_px =
                dof_active
                    ? base_radial_px *
                          std::clamp(layer.blur_strength, 0.0f, 1.0f) *
                          dof_depth_t
                    : 0.0f;

            const float chromatic_px =
                compute_chromatic_px(layer_distance,
                                     normalized_distance,
                                     foreground_layer,
                                     false);

            const bool needs_effect =
                radial_px > kEffectEpsilon ||
                chromatic_px > kEffectEpsilon;

            SDL_Texture* output = fogged_source;

            if (needs_effect) {
                const float quality =
                    foreground_layer
                        ? 1.0f
                        : compute_background_quality_scale(width_,
                                                           height_,
                                                           std::max(radial_px, base_radial_px * 0.20f),
                                                           lens_depth_t);

                if (!process_layer_effects(fogged_source,
                                           layer_effect_target_,
                                           blur_work_,
                                           chain_temp_,
                                           radial_px,
                                           quality,
                                           0.0f,
                                           chromatic_px,
                                           foreground_layer)) {
                    return false;
                }

                output = layer_effect_target_;

                if (radial_px > kEffectEpsilon) {
                    ++result.blur_pass_count;
                }
            }

            return composite_texture_over(output, target);
        };

    if (background_seed) {
        const int seed_distance = std::max(4, max_layer_distance);
        const float seed_norm = 1.0f;

        SDL_Texture* seed_with_fog = chromatic_work_;
        if (!add_layer_depth_fog(renderer_,
                                 background_seed,
                                 seed_with_fog,
                                 width_,
                                 height_,
                                 focus_depth_layer + seed_distance,
                                 focus_depth_layer,
                                 seed_distance)) {
            return fail();
        }

        const float seed_radial =
            dof_active ? base_radial_px * 0.70f * dof_realism_depth_t(seed_distance, seed_norm) : 0.0f;

        const float seed_chromatic =
            compute_chromatic_px(seed_distance, seed_norm, false, true);

        const float seed_quality = std::clamp(
            compute_background_quality_scale(width_, height_, std::max(seed_radial, base_radial_px), 1.0f) *
                layer_effect_tuning::kBackgroundSeedQualityMultiplier,
            layer_effect_tuning::kMinProcessQualityScale,
            1.0f);

        if (seed_radial > kEffectEpsilon || seed_chromatic > kEffectEpsilon) {
            if (!process_layer_effects(seed_with_fog,
                                       background_mid_,
                                       blur_work_,
                                       chain_temp_,
                                       seed_radial,
                                       seed_quality,
                                       0.0f,
                                       seed_chromatic,
                                       false)) {
                return fail();
            }

            if (seed_radial > kEffectEpsilon) {
                ++result.blur_pass_count;
            }
        } else if (!copy_texture(seed_with_fog, background_mid_)) {
            return fail();
        }

        background_has_content = true;
    }

    for (const LayerTexture& layer : scratch_background_layers_) {
        if (!process_and_composite_layer(layer, background_mid_, false)) {
            return fail();
        }

        background_has_content = true;
    }

    for (const LayerTexture& layer : layers) {
        if (layer.depth_layer != focus_depth_layer || !layer.texture) {
            continue;
        }

        SDL_Texture* source = nullptr;

        if (!resolve_layer_source(layer, source)) {
            return fail();
        }

        SDL_Texture* fogged_source = chromatic_work_;
        if (!add_layer_depth_fog(renderer_,
                                 source,
                                 fogged_source,
                                 width_,
                                 height_,
                                 layer.depth_layer,
                                 focus_depth_layer,
                                 max_layer_distance)) {
            return fail();
        }

        constexpr float kFocusLayerRadialMultiplier = 0.025f;
        constexpr float kFocusLayerChromaticMultiplier = 0.020f;

        const float focus_radial_px =
            dof_active
                ? base_radial_px *
                      std::clamp(layer.blur_strength, 0.0f, 1.0f) *
                      kFocusLayerRadialMultiplier
                : 0.0f;

        const int focus_fake_distance = 1;
        const float focus_fake_normalized_distance = 0.08f;

        const float focus_chromatic_px =
            compute_chromatic_px(focus_fake_distance,
                                 focus_fake_normalized_distance,
                                 false,
                                 false) *
            kFocusLayerChromaticMultiplier;

        SDL_Texture* output = fogged_source;

        if (focus_radial_px > kEffectEpsilon ||
            focus_chromatic_px > kEffectEpsilon) {
            if (!process_layer_effects(fogged_source,
                                       layer_effect_target_,
                                       blur_work_,
                                       chain_temp_,
                                       focus_radial_px,
                                       1.0f,
                                       0.0f,
                                       focus_chromatic_px,
                                       false)) {
                return fail();
            }

            output = layer_effect_target_;

            if (focus_radial_px > kEffectEpsilon) {
                ++result.blur_pass_count;
            }
        }

        if (!composite_texture_over(output, background_mid_)) {
            return fail();
        }

        background_has_content = true;
    }

    for (const LayerTexture& layer : scratch_foreground_layers_) {
        if (!process_and_composite_layer(layer, foreground_mid_, true)) {
            return fail();
        }

        foreground_has_content = true;
    }

    result.valid = background_has_content || foreground_has_content;
    result.background_mid = background_has_content ? background_mid_ : nullptr;
    result.foreground_mid = foreground_has_content ? foreground_mid_ : nullptr;

    return finish(result);
}

} // namespace dof_blur_chain