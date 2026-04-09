#include "rendering/render/layer_effect_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "utils/log.hpp"

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInvSqrt2 = 0.70710678f;
constexpr float kMinimumBlurRadiusEpsilonPx = 1.0e-4f;
constexpr int kFogBandTextureMinWidth = 1024;
constexpr int kFogBandTextureHeight = 192;
constexpr int kFogBandWidthBucket = 256;
constexpr float kFogCycleOffsetA = 0.318309886f;
constexpr float kFogCycleOffsetB = 0.141421356f;

struct TextureStateSnapshot {
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    Uint8 alpha_mod = 255;
    Uint8 color_mod_r = 255;
    Uint8 color_mod_g = 255;
    Uint8 color_mod_b = 255;
};

static bool query_texture_size(SDL_Texture* texture, int& out_w, int& out_h) {
    out_w = 0;
    out_h = 0;
    if (!texture) {
        return false;
    }

    float wf = 0.0f;
    float hf = 0.0f;
    if (!SDL_GetTextureSize(texture, &wf, &hf)) {
        return false;
    }

    const int rounded_w = static_cast<int>(std::lround(wf));
    const int rounded_h = static_cast<int>(std::lround(hf));
    if (rounded_w <= 0 || rounded_h <= 0) {
        return false;
    }

    out_w = rounded_w;
    out_h = rounded_h;
    return true;
}

static float smoothstep(float edge0, float edge1, float value) {
    if (edge1 <= edge0) {
        return value >= edge1 ? 1.0f : 0.0f;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - (2.0f * t));
}

static int ceil_to_bucket(int value, int bucket) {
    if (value <= 0 || bucket <= 1) {
        return std::max(1, value);
    }
    return ((value + bucket - 1) / bucket) * bucket;
}

static float fract(float value) {
    const float floor_value = std::floor(value);
    return value - floor_value;
}

static int positive_mod(int value, int modulus) {
    if (modulus <= 0) {
        return 0;
    }
    int result = value % modulus;
    if (result < 0) {
        result += modulus;
    }
    return result;
}

static TextureStateSnapshot capture_texture_state(SDL_Texture* texture) {
    TextureStateSnapshot state{};
    if (!texture) {
        return state;
    }
    SDL_GetTextureBlendMode(texture, &state.blend_mode);
    SDL_GetTextureAlphaMod(texture, &state.alpha_mod);
    SDL_GetTextureColorMod(texture, &state.color_mod_r, &state.color_mod_g, &state.color_mod_b);
    return state;
}

static void clear_target(SDL_Renderer* renderer, SDL_Texture* target) {
    SDL_SetRenderTarget(renderer, target);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
}

static void restore_texture_state(SDL_Texture* texture, const TextureStateSnapshot& state) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, state.blend_mode);
    SDL_SetTextureAlphaMod(texture, state.alpha_mod);
    SDL_SetTextureColorMod(texture, state.color_mod_r, state.color_mod_g, state.color_mod_b);
}

static SDL_BlendMode make_sum_blend_mode() {
    return SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD
    );
}

static void build_gaussian_weights(std::vector<float>& weights,
                                   int kernel_radius,
                                   float sigma) {
    weights.assign(static_cast<std::size_t>(kernel_radius + 1), 0.0f);

    float sum = 0.0f;
    for (int i = 0; i <= kernel_radius; ++i) {
        const float x = static_cast<float>(i);
        const float w = std::exp(-(x * x) / (2.0f * sigma * sigma));
        weights[static_cast<std::size_t>(i)] = w;
        sum += (i == 0) ? w : (2.0f * w);
    }

    if (sum > 1e-6f) {
        for (float& w : weights) {
            w /= sum;
        }
    }
}

static void draw_weighted_offset_sample(SDL_Renderer* renderer,
                                        SDL_Texture* texture,
                                        int draw_w,
                                        int draw_h,
                                        float offset_x,
                                        float offset_y,
                                        float weight) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || weight <= 0.0f) {
        return;
    }

    const int alpha_i = static_cast<int>(std::lround(weight * 255.0f));
    if (alpha_i <= 0) {
        return;
    }

    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(std::clamp(alpha_i, 0, 255)));

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

    SDL_RenderTexture(renderer, texture, &src_rect, &dst_rect);
}

static void draw_weighted_scaled_sample(SDL_Renderer* renderer,
                                        SDL_Texture* texture,
                                        int draw_w,
                                        int draw_h,
                                        const SDL_FPoint& optical_center,
                                        float scale,
                                        float weight) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || weight <= 0.0f) {
        return;
    }

    const int alpha_i = static_cast<int>(std::lround(weight * 255.0f));
    if (alpha_i <= 0) {
        return;
    }

    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(std::clamp(alpha_i, 0, 255)));

    const float scaled_w = static_cast<float>(draw_w) * scale;
    const float scaled_h = static_cast<float>(draw_h) * scale;

    const SDL_FRect src_rect{
        0.0f,
        0.0f,
        static_cast<float>(draw_w),
        static_cast<float>(draw_h)
    };
    const SDL_FRect dst_rect{
        optical_center.x - optical_center.x * scale,
        optical_center.y - optical_center.y * scale,
        scaled_w,
        scaled_h
    };

    SDL_RenderTexture(renderer, texture, &src_rect, &dst_rect);
}

static bool apply_outline_soften_prefill(SDL_Renderer* renderer,
                                         SDL_Texture* src,
                                         SDL_Texture* dst,
                                         int draw_w,
                                         int draw_h,
                                         float soften_radius_px,
                                         SDL_BlendMode sum_blend) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0) {
        return false;
    }
    if (src == dst) {
        return false;
    }

    clear_target(renderer, dst);

    if (soften_radius_px <= kMinimumBlurRadiusEpsilonPx) {
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);

        const SDL_FRect rect{
            0.0f,
            0.0f,
            static_cast<float>(draw_w),
            static_cast<float>(draw_h)
        };
        SDL_RenderTexture(renderer, src, &rect, &rect);
        return true;
    }

    SDL_SetTextureBlendMode(src, sum_blend);

    const float clamped_radius = std::clamp(soften_radius_px, kMinimumBlurRadiusEpsilonPx, 4.5f);
    const int ring_samples = 16;

    const float center_raw_weight = 0.22f;
    const float inner_raw_weight = 0.95f;
    const float outer_raw_weight = 1.30f;

    const float inner_radius = clamped_radius * 0.85f;
    const float outer_radius = clamped_radius * 1.65f;

    const float total_raw_weight =
        center_raw_weight +
        static_cast<float>(ring_samples) * inner_raw_weight +
        static_cast<float>(ring_samples) * outer_raw_weight;

    if (total_raw_weight <= 1e-6f) {
        return false;
    }

    draw_weighted_offset_sample(
        renderer,
        src,
        draw_w,
        draw_h,
        0.0f,
        0.0f,
        center_raw_weight / total_raw_weight
    );

    for (int i = 0; i < ring_samples; ++i) {
        const float angle = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(ring_samples);
        const float cos_a = std::cos(angle);
        const float sin_a = std::sin(angle);

        draw_weighted_offset_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            cos_a * inner_radius,
            sin_a * inner_radius,
            inner_raw_weight / total_raw_weight
        );

        const float angle_outer = angle + (kPi / static_cast<float>(ring_samples));
        draw_weighted_offset_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            std::cos(angle_outer) * outer_radius,
            std::sin(angle_outer) * outer_radius,
            outer_raw_weight / total_raw_weight
        );
    }

    return true;
}

static bool apply_axis_blur(SDL_Renderer* renderer,
                            SDL_Texture* src,
                            SDL_Texture* dst,
                            int draw_w,
                            int draw_h,
                            float radius_px,
                            float dir_x,
                            float dir_y,
                            SDL_BlendMode sum_blend) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0) {
        return false;
    }
    if (src == dst) {
        return false;
    }

    clear_target(renderer, dst);

    if (radius_px <= kMinimumBlurRadiusEpsilonPx) {
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);

        const SDL_FRect rect{
            0.0f,
            0.0f,
            static_cast<float>(draw_w),
            static_cast<float>(draw_h)
        };
        SDL_RenderTexture(renderer, src, &rect, &rect);
        return true;
    }

    const int kernel_radius = std::clamp(static_cast<int>(std::ceil(radius_px)), 1, 24);
    const float sigma = std::max(kMinimumBlurRadiusEpsilonPx, radius_px * 0.58f);

    std::vector<float> weights;
    build_gaussian_weights(weights, kernel_radius, sigma);

    SDL_SetTextureBlendMode(src, sum_blend);

    for (int i = -kernel_radius; i <= kernel_radius; ++i) {
        const float weight = weights[static_cast<std::size_t>(std::abs(i))];
        draw_weighted_offset_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            static_cast<float>(i) * dir_x,
            static_cast<float>(i) * dir_y,
            weight
        );
    }

    return true;
}

static bool apply_radial_zoom_blur(SDL_Renderer* renderer,
                                   SDL_Texture* src,
                                   SDL_Texture* dst,
                                   int draw_w,
                                   int draw_h,
                                   float radial_radius_px,
                                   const SDL_FPoint& optical_center,
                                   SDL_BlendMode sum_blend) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0) {
        return false;
    }
    if (src == dst) {
        return false;
    }

    clear_target(renderer, dst);

    if (radial_radius_px <= kMinimumBlurRadiusEpsilonPx) {
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);

        const SDL_FRect rect{
            0.0f,
            0.0f,
            static_cast<float>(draw_w),
            static_cast<float>(draw_h)
        };
        SDL_RenderTexture(renderer, src, &rect, &rect);
        return true;
    }

    SDL_SetTextureBlendMode(src, sum_blend);

    const float max_dim = static_cast<float>(std::max(draw_w, draw_h));
    const float normalized_radius = std::clamp(radial_radius_px / std::max(1.0f, max_dim), 0.0f, 1.0f);
    const int sample_count = std::clamp(static_cast<int>(std::ceil(radial_radius_px * 2.8f)), 2, 72);
    const float max_scale_delta = std::min(0.72f, normalized_radius * 4.2f);

    const float base_raw_weight = 0.92f;
    constexpr float kOutwardWeightScale = 1.55f;
    constexpr float kInwardWeightScale = 0.45f;
    float total_raw_weight = base_raw_weight;

    std::vector<float> side_weights(static_cast<std::size_t>(sample_count), 0.0f);
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float w = std::pow(1.0f - t, 1.1f);
        side_weights[static_cast<std::size_t>(i)] = w;
        total_raw_weight += (kOutwardWeightScale + kInwardWeightScale) * w;
    }

    if (total_raw_weight <= 1e-6f) {
        return false;
    }

    draw_weighted_scaled_sample(
        renderer,
        src,
        draw_w,
        draw_h,
        optical_center,
        1.0f,
        base_raw_weight / total_raw_weight
    );

    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float edge_bias = 0.25f + (0.75f * t);
        const float scale_delta = max_scale_delta * edge_bias;
        const float raw_weight = side_weights[static_cast<std::size_t>(i)];
        const float inward_weight = (raw_weight * kInwardWeightScale) / total_raw_weight;
        const float outward_weight = (raw_weight * kOutwardWeightScale) / total_raw_weight;

        draw_weighted_scaled_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            optical_center,
            std::max(0.05f, 1.0f - (scale_delta * 0.45f)),
            inward_weight
        );

        draw_weighted_scaled_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            optical_center,
            1.0f + scale_delta,
            outward_weight
        );
    }

    return true;
}

} // namespace

LayerEffectProcessor::~LayerEffectProcessor() {
    destroy_owned_resources();
}

void LayerEffectProcessor::set_renderer(SDL_Renderer* renderer) {
    if (renderer_ == renderer) {
        return;
    }
    destroy_owned_resources();
    renderer_ = renderer;
}

double LayerEffectProcessor::radial_lens_factor_from_optics(double focal_length_mm, double f_stop) {
    const double safe_f_stop = std::max(0.01, f_stop);
    const double ratio = std::max(0.0, focal_length_mm / safe_f_stop);
    return std::clamp(ratio / 24.0, 0.25, 2.0);
}

double LayerEffectProcessor::coc_blur_radius_from_depth_delta(double depth_delta,
                                                              double max_cull_depth,
                                                              double focal_length_mm,
                                                              double f_stop,
                                                              double max_blur_px) {
    const double safe_max_blur = std::max(0.0, max_blur_px);
    if (safe_max_blur <= 0.0) {
        return 0.0;
    }

    const double safe_max_depth = std::max(1.0, max_cull_depth);
    const double safe_f_stop = std::max(0.01, f_stop);
    const double safe_focal = std::max(0.01, focal_length_mm);

    const double aperture_diameter_mm = safe_focal / safe_f_stop;
    const double optical_scale = std::clamp(aperture_diameter_mm / 18.0, 0.08, 2.5);
    const double normalized_delta = std::clamp(std::fabs(depth_delta) / safe_max_depth, 0.0, 1.0);
    const double depth_scale = std::pow(normalized_delta, 1.1);

    return std::clamp(safe_max_blur * optical_scale * depth_scale, 0.0, safe_max_blur);
}

bool LayerEffectProcessor::apply_lens_blur(SDL_Texture* src,
                                           SDL_Texture* dst,
                                           SDL_Texture* scratch,
                                           int target_w,
                                           int target_h,
                                           float radius_px,
                                           const SDL_FPoint& optical_center,
                                           float radial_radius_px,
                                           float quality_scale) const {
    if (!renderer_ || !src || !dst || !scratch || target_w <= 0 || target_h <= 0) {
        return false;
    }
    if (scratch == src || scratch == dst) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);

    const TextureStateSnapshot src_state = capture_texture_state(src);
    const TextureStateSnapshot scratch_state = capture_texture_state(scratch);
    const TextureStateSnapshot dst_state = capture_texture_state(dst);

    const SDL_BlendMode sum_blend = make_sum_blend_mode();
    if (sum_blend == SDL_BLENDMODE_INVALID) {
        SDL_SetRenderTarget(renderer_, dst);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);
        SDL_SetTextureColorMod(src, 255, 255, 255);
        SDL_RenderTexture(renderer_, src, nullptr, nullptr);

        restore_texture_state(src, src_state);
        restore_texture_state(scratch, scratch_state);
        restore_texture_state(dst, dst_state);
        SDL_SetRenderTarget(renderer_, previous_target);
        return true;
    }

    const float clamped_quality = std::clamp(quality_scale, 0.35f, 1.0f);
    const int process_w = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(target_w) * clamped_quality)),
        1,
        target_w
    );
    const int process_h = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(target_h) * clamped_quality)),
        1,
        target_h
    );
    const bool reduced_resolution = (process_w != target_w) || (process_h != target_h);

    const float process_blur_radius = std::max(0.0f, radius_px) * clamped_quality;
    const float process_radial_radius = std::max(0.0f, radial_radius_px) * clamped_quality;
    const bool has_blur = (process_blur_radius > kMinimumBlurRadiusEpsilonPx) ||
                          (process_radial_radius > kMinimumBlurRadiusEpsilonPx);

    const SDL_FPoint scaled_optical_center{
        optical_center.x * clamped_quality,
        optical_center.y * clamped_quality
    };

    SDL_Texture* current = src;

    if (reduced_resolution) {
        clear_target(renderer_, scratch);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);
        SDL_SetTextureColorMod(src, 255, 255, 255);

        const SDL_FRect lowres_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };
        SDL_RenderTexture(renderer_, src, nullptr, &lowres_rect);
        current = scratch;
    }

    auto pick_other_temp = [&](SDL_Texture* tex) -> SDL_Texture* {
        return (tex == scratch) ? dst : scratch;
    };

    if (has_blur) {
        const float blur_mix = process_blur_radius + process_radial_radius;
        const float outline_soften_radius = std::clamp(
            blur_mix * 0.42f,
            kMinimumBlurRadiusEpsilonPx,
            3.60f
        );

        SDL_Texture* next = pick_other_temp(current);
        if (!apply_outline_soften_prefill(renderer_, current, next, process_w, process_h, outline_soften_radius, sum_blend)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            SDL_SetRenderTarget(renderer_, previous_target);
            return false;
        }
        current = next;
    }

    if (process_blur_radius > kMinimumBlurRadiusEpsilonPx) {
        SDL_Texture* next = pick_other_temp(current);
        if (!apply_axis_blur(renderer_, current, next, process_w, process_h, process_blur_radius, 1.0f, 0.0f, sum_blend)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            SDL_SetRenderTarget(renderer_, previous_target);
            return false;
        }
        current = next;

        next = pick_other_temp(current);
        if (!apply_axis_blur(renderer_, current, next, process_w, process_h, process_blur_radius, 0.0f, 1.0f, sum_blend)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            SDL_SetRenderTarget(renderer_, previous_target);
            return false;
        }
        current = next;

        const float diagonal_radius = process_blur_radius * 0.55f;
        if (diagonal_radius > 0.75f) {
            next = pick_other_temp(current);
            if (!apply_axis_blur(renderer_, current, next, process_w, process_h, diagonal_radius, kInvSqrt2, kInvSqrt2, sum_blend)) {
                restore_texture_state(src, src_state);
                restore_texture_state(scratch, scratch_state);
                restore_texture_state(dst, dst_state);
                SDL_SetRenderTarget(renderer_, previous_target);
                return false;
            }
            current = next;

            next = pick_other_temp(current);
            if (!apply_axis_blur(renderer_, current, next, process_w, process_h, diagonal_radius, kInvSqrt2, -kInvSqrt2, sum_blend)) {
                restore_texture_state(src, src_state);
                restore_texture_state(scratch, scratch_state);
                restore_texture_state(dst, dst_state);
                SDL_SetRenderTarget(renderer_, previous_target);
                return false;
            }
            current = next;
        }
    }

    if (process_radial_radius > kMinimumBlurRadiusEpsilonPx) {
        SDL_Texture* next = pick_other_temp(current);
        if (!apply_radial_zoom_blur(renderer_, current, next, process_w, process_h, process_radial_radius, scaled_optical_center, sum_blend)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            SDL_SetRenderTarget(renderer_, previous_target);
            return false;
        }
        current = next;
    }

    if (reduced_resolution) {
        if (current == dst) {
            clear_target(renderer_, scratch);
            SDL_SetTextureBlendMode(dst, SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(dst, 255);
            SDL_SetTextureColorMod(dst, 255, 255, 255);

            const SDL_FRect lowres_rect{
                0.0f,
                0.0f,
                static_cast<float>(process_w),
                static_cast<float>(process_h)
            };
            SDL_RenderTexture(renderer_, dst, &lowres_rect, &lowres_rect);
            current = scratch;
        }

        clear_target(renderer_, dst);
        SDL_SetTextureBlendMode(current, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(current, 255);
        SDL_SetTextureColorMod(current, 255, 255, 255);

        const SDL_FRect lowres_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };
        SDL_RenderTexture(renderer_, current, &lowres_rect, nullptr);
    } else if (current != dst) {
        clear_target(renderer_, dst);
        SDL_SetTextureBlendMode(current, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(current, 255);
        SDL_SetTextureColorMod(current, 255, 255, 255);
        SDL_RenderTexture(renderer_, current, nullptr, nullptr);
    }

    if (has_blur) {
        const float blur_strength = std::clamp(
            (process_blur_radius + process_radial_radius) / 24.0f,
            0.0f,
            1.0f
        );

        if (blur_strength < 0.18f) {
            const float reinforce_alpha_f = 0.03f + (0.05f * (1.0f - blur_strength));
            const Uint8 reinforce_alpha = static_cast<Uint8>(
                std::clamp(static_cast<int>(std::lround(reinforce_alpha_f * 255.0f)), 0, 255)
            );

            if (reinforce_alpha > 0) {
                SDL_SetRenderTarget(renderer_, dst);
                SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
                SDL_SetTextureAlphaMod(src, reinforce_alpha);
                SDL_SetTextureColorMod(src, 255, 255, 255);
                SDL_RenderTexture(renderer_, src, nullptr, nullptr);
            }
        }
    }

    restore_texture_state(src, src_state);
    restore_texture_state(scratch, scratch_state);
    restore_texture_state(dst, dst_state);
    SDL_SetRenderTarget(renderer_, previous_target);
    return true;
}

LayerEffectProcessor::LayerProcessResult LayerEffectProcessor::process_layer(
    SDL_Texture* base_layer_texture,
    SDL_Texture* composited_output_texture,
    double layer_depth_min,
    double layer_depth_max,
    const LayerLightingParams& lighting_params,
    const std::vector<RuntimeLight>& lights,
    const LayerFogParams& fog_params,
    const LayerScratchTextures& scratch_textures) {
    LayerProcessResult result{};
    result.final_texture = composited_output_texture;

    if (!renderer_ || !base_layer_texture || !composited_output_texture) {
        return result;
    }

    int target_w = 0;
    int target_h = 0;
    if (!query_texture_size(composited_output_texture, target_w, target_h) || target_w <= 0 || target_h <= 0) {
        return result;
    }

    int dark_mask_w = 0;
    int dark_mask_h = 0;
    const bool dark_mask_ready = scratch_textures.dark_mask_texture &&
                                 query_texture_size(scratch_textures.dark_mask_texture, dark_mask_w, dark_mask_h) &&
                                 dark_mask_w == target_w &&
                                 dark_mask_h == target_h;

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);

    const TextureStateSnapshot base_state = capture_texture_state(base_layer_texture);
    const TextureStateSnapshot output_state = capture_texture_state(composited_output_texture);
    TextureStateSnapshot dark_mask_state{};
    if (scratch_textures.dark_mask_texture) {
        dark_mask_state = capture_texture_state(scratch_textures.dark_mask_texture);
    }

    auto restore_state_and_target = [&]() {
        restore_texture_state(base_layer_texture, base_state);
        restore_texture_state(composited_output_texture, output_state);
        if (scratch_textures.dark_mask_texture) {
            restore_texture_state(scratch_textures.dark_mask_texture, dark_mask_state);
        }
        SDL_SetRenderTarget(renderer_, previous_target);
    };

    // Step 1: base layer render.
    clear_target(renderer_, composited_output_texture);
    SDL_SetTextureBlendMode(base_layer_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(base_layer_texture, 255);
    SDL_SetTextureColorMod(base_layer_texture, 255, 255, 255);
    SDL_RenderTexture(renderer_, base_layer_texture, nullptr, nullptr);

    // Steps 2-4: dark mask generation, light application, and mask composite.
    if (lighting_params.enabled && dark_mask_ready) {
        if (!supports_strict_dark_mask_pipeline()) {
            if (!warned_missing_strict_dark_mask_pipeline_blend_modes_) {
                vibble::log::warn("[LayerEffectProcessor] Strict alpha-preserving dark-mask blends unavailable; skipping dark-mask lighting.");
                warned_missing_strict_dark_mask_pipeline_blend_modes_ = true;
            }
        } else {
            const SDL_BlendMode alpha_copy = alpha_copy_blend_mode();
            const SDL_BlendMode light_add_preserve_alpha = light_add_rgb_preserve_alpha_blend_mode();
            const SDL_BlendMode alpha_masked_multiply = alpha_masked_multiply_blend_mode();

            SDL_SetRenderTarget(renderer_, scratch_textures.dark_mask_texture);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_,
                                   lighting_params.ambient_color.r,
                                   lighting_params.ambient_color.g,
                                   lighting_params.ambient_color.b,
                                   0);
            SDL_RenderClear(renderer_);

            // Keep ambient color, copy only source alpha from base layer.
            SDL_SetTextureBlendMode(base_layer_texture, alpha_copy);
            SDL_SetTextureAlphaMod(base_layer_texture, 255);
            SDL_SetTextureColorMod(base_layer_texture, 255, 255, 255);
            SDL_RenderTexture(renderer_, base_layer_texture, nullptr, nullptr);

            bool drew_light_energy = false;
            if (!lights.empty()) {
                SDL_SetRenderTarget(renderer_, scratch_textures.dark_mask_texture);
                SDL_Texture* last_falloff_texture = nullptr;
                SDL_Color last_color{0, 0, 0, 0};
                Uint8 last_alpha = 0;
                for (const RuntimeLight& light : lights) {
                    const float behind_weight = behind_occlusion_weight(light.world_z,
                                                                        layer_depth_min,
                                                                        layer_depth_max,
                                                                        light.radius_px);
                    if (behind_weight < 0.02f) {
                        continue;
                    }

                    const float effective_intensity = std::max(0.0f, light.intensity) * behind_weight;
                    if (effective_intensity <= 0.0005f) {
                        continue;
                    }

                    SDL_Texture* falloff_texture = ensure_light_falloff_texture(light.falloff);
                    if (!falloff_texture) {
                        continue;
                    }

                    const int pass_count = std::clamp(static_cast<int>(std::ceil(effective_intensity)), 1, 4);
                    const float per_pass = effective_intensity / static_cast<float>(pass_count);
                    const Uint8 alpha = static_cast<Uint8>(std::clamp(
                        static_cast<int>(std::lround(per_pass * 255.0f)),
                        0,
                        255));
                    if (alpha == 0) {
                        continue;
                    }

                    if (falloff_texture != last_falloff_texture) {
                        SDL_SetTextureBlendMode(falloff_texture, light_add_preserve_alpha);
                        last_falloff_texture = falloff_texture;
                        last_color = SDL_Color{0, 0, 0, 0};
                        last_alpha = 0;
                    }
                    if (last_color.r != light.color.r ||
                        last_color.g != light.color.g ||
                        last_color.b != light.color.b) {
                        SDL_SetTextureColorMod(falloff_texture, light.color.r, light.color.g, light.color.b);
                        last_color = SDL_Color{light.color.r, light.color.g, light.color.b, 255};
                    }
                    if (last_alpha != alpha) {
                        SDL_SetTextureAlphaMod(falloff_texture, alpha);
                        last_alpha = alpha;
                    }

                    const float radius = std::max(4.0f, light.radius_px);
                    const SDL_FRect light_dst{
                        light.screen_center.x - radius,
                        light.screen_center.y - radius,
                        radius * 2.0f,
                        radius * 2.0f
                    };

                    for (int pass = 0; pass < pass_count; ++pass) {
                        SDL_RenderTexture(renderer_, falloff_texture, nullptr, &light_dst);
                    }
                    drew_light_energy = true;
                }

                if (drew_light_energy) {
                    // Re-copy base alpha after additive lighting to guarantee exact alpha preservation.
                    SDL_SetTextureBlendMode(base_layer_texture, alpha_copy);
                    SDL_SetTextureAlphaMod(base_layer_texture, 255);
                    SDL_SetTextureColorMod(base_layer_texture, 255, 255, 255);
                    SDL_RenderTexture(renderer_, base_layer_texture, nullptr, nullptr);
                }
            }

            SDL_SetRenderTarget(renderer_, composited_output_texture);
            SDL_SetTextureBlendMode(scratch_textures.dark_mask_texture, alpha_masked_multiply);
            SDL_SetTextureAlphaMod(scratch_textures.dark_mask_texture, 255);
            SDL_SetTextureColorMod(scratch_textures.dark_mask_texture, 255, 255, 255);
            SDL_RenderTexture(renderer_, scratch_textures.dark_mask_texture, nullptr, nullptr);

            result.lighting_applied = true;
        }
    }

    // Step 5: fog over composited layer only.
    if (fog_params.enabled) {
        const float fog_bottom = std::clamp(fog_params.bottom_y_px, 0.0f, static_cast<float>(target_h));
        if (fog_bottom > 1.0f &&
            ensure_fog_band_textures(target_w, fog_params.bottom_opacity_curve)) {
            const int cycle_index = fog_params.layer_cycle_index;
            const int variant_index = positive_mod(cycle_index, LayerEffectProcessor::kFogVariantCount);
            SDL_Texture* fog_texture = fog_band_texture_for_cycle(variant_index);
            int fog_w = 0;
            int fog_h = 0;
            if (fog_texture && query_texture_size(fog_texture, fog_w, fog_h) && fog_w > 0 && fog_h > 0) {
                const float depth_t = std::clamp(fog_params.normalized_depth, 0.0f, 1.0f);
                const float thickness = std::clamp(fog_params.thickness, 0.0f, 4.0f);
                const float shaped = std::pow(depth_t, 0.68f);
                const float depth_coverage = smoothstep(0.02f, 0.98f, shaped);
                float fog_strength = std::clamp(depth_coverage * (0.46f + (0.62f * thickness)), 0.0f, 1.0f);
                if (depth_t >= 0.985f) {
                    fog_strength = 1.0f;
                }
                const Uint8 fog_alpha = static_cast<Uint8>(std::clamp(
                    static_cast<int>(std::lround(fog_strength * 255.0f)),
                    0,
                    255));
                if (fog_alpha > 0) {
                    SDL_SetRenderTarget(renderer_, composited_output_texture);
                    SDL_SetTextureBlendMode(fog_texture, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureColorMod(fog_texture, fog_params.tint.r, fog_params.tint.g, fog_params.tint.b);
                    SDL_SetTextureAlphaMod(fog_texture, fog_alpha);

                    const float sample_w = std::max(1.0f, std::min(static_cast<float>(target_w), static_cast<float>(fog_w)));
                    const float sample_h = static_cast<float>(fog_h);
                    const float max_src_x = std::max(0.0f, static_cast<float>(fog_w) - sample_w);
                    const float offset_t = fract((static_cast<float>(cycle_index) * kFogCycleOffsetA) +
                                                 (static_cast<float>(variant_index) * kFogCycleOffsetB));
                    const float base_src_x = max_src_x * offset_t;
                    const SDL_FRect base_src_rect{
                        base_src_x,
                        0.0f,
                        sample_w,
                        sample_h
                    };
                    const SDL_FRect dst_rect{
                        0.0f,
                        0.0f,
                        static_cast<float>(target_w),
                        fog_bottom
                    };
                    SDL_RenderTexture(renderer_, fog_texture, &base_src_rect, &dst_rect);

                    const Uint8 detail_alpha = static_cast<Uint8>(std::clamp(
                        static_cast<int>(std::lround(static_cast<float>(fog_alpha) * (0.32f + (0.22f * depth_t)))),
                        0,
                        255));
                    if (detail_alpha > 0) {
                        const float detail_min_w = std::min(16.0f, sample_w);
                        const float detail_sample_w = std::clamp(sample_w * 0.88f, detail_min_w, sample_w);
                        const float detail_max_src_x = std::max(0.0f, static_cast<float>(fog_w) - detail_sample_w);
                        const float detail_offset_t = fract(offset_t + 0.17320508f + (depth_t * 0.11f));
                        const SDL_FRect detail_src_rect{
                            detail_max_src_x * detail_offset_t,
                            0.0f,
                            detail_sample_w,
                            sample_h
                        };
                        SDL_SetTextureAlphaMod(fog_texture, detail_alpha);
                        SDL_RenderTexture(renderer_, fog_texture, &detail_src_rect, &dst_rect);
                    }

                    SDL_SetTextureAlphaMod(fog_texture, 255);
                    SDL_SetTextureColorMod(fog_texture, 255, 255, 255);
                    result.fog_applied = true;
                }
            }
        }
    }

    restore_state_and_target();
    return result;
}

void LayerEffectProcessor::destroy_owned_resources() {
    destroy_fog_resources();
    destroy_lighting_resources();
}

void LayerEffectProcessor::destroy_fog_resources() {
    for (SDL_Texture*& texture : fog_band_textures_) {
        if (texture) {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
    }
    fog_band_baked_width_ = 0;
    fog_band_baked_height_ = 0;
    fog_band_baked_curve_ = 1.0f;
}

void LayerEffectProcessor::destroy_lighting_resources() {
    if (light_accum_texture_) {
        SDL_DestroyTexture(light_accum_texture_);
        light_accum_texture_ = nullptr;
    }
    light_accum_width_ = 0;
    light_accum_height_ = 0;

    for (auto& [key, texture] : light_falloff_textures_) {
        (void)key;
        if (texture) {
            SDL_DestroyTexture(texture);
        }
    }
    light_falloff_textures_.clear();

    alpha_copy_blend_mode_ = SDL_BLENDMODE_INVALID;
    light_add_rgb_preserve_alpha_blend_mode_ = SDL_BLENDMODE_INVALID;
    alpha_masked_multiply_blend_mode_ = SDL_BLENDMODE_INVALID;
    alpha_copy_blend_mode_ready_ = false;
    light_add_rgb_preserve_alpha_blend_mode_ready_ = false;
    alpha_masked_multiply_blend_mode_ready_ = false;

    warned_missing_alpha_copy_blend_mode_ = false;
    warned_missing_strict_dark_mask_pipeline_blend_modes_ = false;
}

bool LayerEffectProcessor::ensure_fog_band_textures(int target_w, float bottom_opacity_curve) {
    if (!renderer_ || target_w <= 0) {
        return false;
    }

    const float clamped_curve = std::clamp(bottom_opacity_curve, 0.25f, 3.0f);
    const int requested_width = ceil_to_bucket(std::max(kFogBandTextureMinWidth, target_w * 2), kFogBandWidthBucket);
    const bool all_textures_ready = std::all_of(fog_band_textures_.begin(),
                                                fog_band_textures_.end(),
                                                [](SDL_Texture* tex) { return tex != nullptr; });
    const bool size_ok = fog_band_baked_width_ >= requested_width && fog_band_baked_height_ == kFogBandTextureHeight;
    const bool curve_ok = std::fabs(fog_band_baked_curve_ - clamped_curve) <= 1e-4f;
    if (all_textures_ready && size_ok && curve_ok) {
        return true;
    }

    const int bake_width = std::max(requested_width, fog_band_baked_width_);
    destroy_fog_resources();

    const SDL_PixelFormatDetails* pixel_format = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA8888);
    if (!pixel_format) {
        return false;
    }

    for (int variant = 0; variant < kFogVariantCount; ++variant) {
        SDL_Texture* texture = SDL_CreateTexture(renderer_,
                                                 SDL_PIXELFORMAT_RGBA8888,
                                                 SDL_TEXTUREACCESS_STREAMING,
                                                 bake_width,
                                                 kFogBandTextureHeight);
        if (!texture) {
            destroy_fog_resources();
            return false;
        }

        void* pixels = nullptr;
        int pitch = 0;
        if (!SDL_LockTexture(texture, nullptr, &pixels, &pitch) || !pixels || pitch <= 0) {
            SDL_DestroyTexture(texture);
            destroy_fog_resources();
            return false;
        }

        auto* base = static_cast<std::uint8_t*>(pixels);
        const float variant_phase = static_cast<float>(variant) * 1.61803399f;
        for (int y = 0; y < kFogBandTextureHeight; ++y) {
            auto* row = reinterpret_cast<Uint32*>(base + (pitch * y));
            const float v = static_cast<float>(y) / static_cast<float>(kFogBandTextureHeight - 1);
            const float d = 1.0f - v; // 0 at cutoff, 1 toward horizon.
            const float ramp = std::pow(std::clamp(d, 0.0f, 1.0f), clamped_curve);
            const float bottom_feather = smoothstep(0.0f, 0.06f, d);
            const float vertical_profile = std::clamp(ramp * bottom_feather, 0.0f, 1.0f);

            for (int x = 0; x < bake_width; ++x) {
                const float fx = static_cast<float>(x) / static_cast<float>(std::max(1, bake_width - 1));
                const float domain_x = fx + (0.06f * std::sin((v * 5.4f) + (variant_phase * 0.63f)));
                const float n0 = 0.5f + 0.5f * std::sin((domain_x * 21.3f) + (v * 3.8f) + (variant_phase * 0.73f));
                const float n1 = 0.5f + 0.5f * std::sin((domain_x * 47.1f) - (v * 11.7f) + (variant_phase * 1.21f));
                const float n2 = 0.5f + 0.5f * std::sin(((domain_x + (v * 0.9f)) * 83.5f) + (variant_phase * 1.57f));
                const float n3 = 0.5f + 0.5f * std::sin(((domain_x * 7.7f) - (v * 14.1f)) + (variant_phase * 0.29f));
                const float base_noise = std::clamp(
                    (n0 * 0.40f) + (n1 * 0.28f) + (n2 * 0.20f) + (n3 * 0.12f),
                    0.0f,
                    1.0f);

                const float ridged = 1.0f - std::fabs((base_noise * 2.0f) - 1.0f);
                const float wisps = std::pow(std::clamp(ridged, 0.0f, 1.0f), 1.5f);
                const float alpha_f = std::clamp(
                    vertical_profile * (0.46f + (0.39f * base_noise) + (0.15f * wisps)),
                    0.0f,
                    1.0f);
                const Uint8 alpha = static_cast<Uint8>(std::clamp(
                    static_cast<int>(std::lround(alpha_f * 255.0f)),
                    0,
                    255));
                row[x] = SDL_MapRGBA(pixel_format, nullptr, 255, 255, 255, alpha);
            }
        }

        SDL_UnlockTexture(texture);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        fog_band_textures_[static_cast<std::size_t>(variant)] = texture;
    }

    fog_band_baked_width_ = bake_width;
    fog_band_baked_height_ = kFogBandTextureHeight;
    fog_band_baked_curve_ = clamped_curve;
    return true;
}

SDL_Texture* LayerEffectProcessor::fog_band_texture_for_cycle(int layer_cycle_index) const {
    const int slot = positive_mod(layer_cycle_index, kFogVariantCount);
    if (slot < 0 || slot >= kFogVariantCount) {
        return nullptr;
    }
    return fog_band_textures_[static_cast<std::size_t>(slot)];
}

bool LayerEffectProcessor::ensure_light_accum_texture(int target_w, int target_h) {
    if (!renderer_ || target_w <= 0 || target_h <= 0) {
        return false;
    }

    if (light_accum_texture_ &&
        light_accum_width_ == target_w &&
        light_accum_height_ == target_h) {
        return true;
    }

    if (light_accum_texture_) {
        SDL_DestroyTexture(light_accum_texture_);
        light_accum_texture_ = nullptr;
    }

    light_accum_texture_ = SDL_CreateTexture(renderer_,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             target_w,
                                             target_h);
    if (!light_accum_texture_) {
        light_accum_width_ = 0;
        light_accum_height_ = 0;
        return false;
    }

    SDL_SetTextureBlendMode(light_accum_texture_, SDL_BLENDMODE_BLEND);
    light_accum_width_ = target_w;
    light_accum_height_ = target_h;
    return true;
}

SDL_Texture* LayerEffectProcessor::ensure_light_falloff_texture(float falloff) {
    if (!renderer_) {
        return nullptr;
    }

    const int quantized_falloff = std::clamp(static_cast<int>(std::lround(falloff * 100.0f)), 5, 800);
    const auto existing = light_falloff_textures_.find(quantized_falloff);
    if (existing != light_falloff_textures_.end()) {
        return existing->second;
    }

    constexpr int kTextureSize = 256;
    SDL_Texture* texture = SDL_CreateTexture(renderer_,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             kTextureSize,
                                             kTextureSize);
    if (!texture) {
        return nullptr;
    }

    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(texture, nullptr, &pixels, &pitch) || !pixels || pitch <= 0) {
        SDL_DestroyTexture(texture);
        return nullptr;
    }

    const float center = (static_cast<float>(kTextureSize) - 1.0f) * 0.5f;
    const float max_radius = std::max(1.0f, center);
    const float exponent = std::clamp(static_cast<float>(quantized_falloff) / 100.0f, 0.05f, 8.0f);
    auto* base = static_cast<std::uint8_t*>(pixels);
    const SDL_PixelFormatDetails* pixel_format = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA8888);
    if (!pixel_format) {
        SDL_UnlockTexture(texture);
        SDL_DestroyTexture(texture);
        return nullptr;
    }
    for (int y = 0; y < kTextureSize; ++y) {
        auto* row = reinterpret_cast<Uint32*>(base + (pitch * y));
        for (int x = 0; x < kTextureSize; ++x) {
            const float dx = (static_cast<float>(x) - center) / max_radius;
            const float dy = (static_cast<float>(y) - center) / max_radius;
            const float distance = std::sqrt((dx * dx) + (dy * dy));
            const float radial = std::clamp(1.0f - distance, 0.0f, 1.0f);
            const float smoothed = radial * radial * (3.0f - (2.0f * radial));
            const float curved = std::pow(smoothed, exponent);
            const Uint8 value = static_cast<Uint8>(std::clamp(
                static_cast<int>(std::lround(curved * 255.0f)),
                0,
                255));
            row[x] = SDL_MapRGBA(pixel_format, nullptr, value, value, value, value);
        }
    }

    SDL_UnlockTexture(texture);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    light_falloff_textures_.emplace(quantized_falloff, texture);
    return texture;
}

SDL_BlendMode LayerEffectProcessor::alpha_copy_blend_mode() {
    if (alpha_copy_blend_mode_ready_) {
        return alpha_copy_blend_mode_;
    }
    alpha_copy_blend_mode_ready_ = true;
    alpha_copy_blend_mode_ = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDOPERATION_ADD);
    return alpha_copy_blend_mode_;
}

SDL_BlendMode LayerEffectProcessor::light_add_rgb_preserve_alpha_blend_mode() {
    if (light_add_rgb_preserve_alpha_blend_mode_ready_) {
        return light_add_rgb_preserve_alpha_blend_mode_;
    }
    light_add_rgb_preserve_alpha_blend_mode_ready_ = true;
    light_add_rgb_preserve_alpha_blend_mode_ = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    return light_add_rgb_preserve_alpha_blend_mode_;
}

SDL_BlendMode LayerEffectProcessor::alpha_masked_multiply_blend_mode() {
    if (alpha_masked_multiply_blend_mode_ready_) {
        return alpha_masked_multiply_blend_mode_;
    }
    alpha_masked_multiply_blend_mode_ready_ = true;
    alpha_masked_multiply_blend_mode_ = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_DST_COLOR,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    return alpha_masked_multiply_blend_mode_;
}

bool LayerEffectProcessor::supports_strict_dark_mask_pipeline() {
    return alpha_copy_blend_mode() != SDL_BLENDMODE_INVALID &&
           light_add_rgb_preserve_alpha_blend_mode() != SDL_BLENDMODE_INVALID &&
           alpha_masked_multiply_blend_mode() != SDL_BLENDMODE_INVALID;
}

float LayerEffectProcessor::behind_occlusion_weight(double light_world_z,
                                                    double layer_depth_min,
                                                    double layer_depth_max,
                                                    float light_radius_px) const {
    const double z_near = std::min(layer_depth_min, layer_depth_max);
    const double z_far = std::max(layer_depth_min, layer_depth_max);
    const double d_behind = std::max(0.0, light_world_z - z_far);
    const double layer_thickness = std::max(1.0, z_far - z_near);
    const double attenuation_span = std::max(
        24.0,
        (0.55 * std::max(1.0f, light_radius_px)) + (0.65 * layer_thickness));

    const double weight = std::exp(-(d_behind / attenuation_span));
    return static_cast<float>(std::clamp(weight, 0.0, 1.0));
}
