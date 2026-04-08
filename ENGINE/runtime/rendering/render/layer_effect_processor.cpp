#include "rendering/render/layer_effect_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "utils/log.hpp"

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInvSqrt2 = 0.70710678f;
constexpr float kMinimumUsefulBlurRadiusPx = 0.35f;
constexpr int kFogBandTextureWidth = 384;
constexpr int kFogBandTextureHeight = 192;

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

    if (soften_radius_px <= 0.01f) {
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

    const float clamped_radius = std::clamp(soften_radius_px, 0.75f, 4.5f);
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

    if (radius_px <= 0.01f) {
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
    const float sigma = std::max(0.85f, radius_px * 0.58f);

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

    if (radial_radius_px <= 0.01f) {
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
    const int sample_count = std::clamp(static_cast<int>(std::ceil(radial_radius_px * 1.35f)), 6, 28);
    const float max_scale_delta = std::min(0.18f, normalized_radius * 1.6f);

    const float base_raw_weight = 2.6f;
    float total_raw_weight = base_raw_weight;

    std::vector<float> side_weights(static_cast<std::size_t>(sample_count), 0.0f);
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float w = std::pow(1.0f - t, 1.35f);
        side_weights[static_cast<std::size_t>(i)] = w;
        total_raw_weight += 2.0f * w;
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
        const float scale_delta = max_scale_delta * t;
        const float weight = side_weights[static_cast<std::size_t>(i)] / total_raw_weight;

        draw_weighted_scaled_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            optical_center,
            std::max(0.05f, 1.0f - scale_delta),
            weight
        );

        draw_weighted_scaled_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            optical_center,
            1.0f + scale_delta,
            weight
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
    const bool has_blur = (process_blur_radius > 0.01f) || (process_radial_radius > 0.01f);

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
        const float outline_soften_radius = std::clamp(
            1.00f + process_blur_radius * 0.24f + process_radial_radius * 0.12f,
            1.10f,
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

    if (process_blur_radius > 0.01f) {
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

    if (process_radial_radius > 0.01f) {
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
    const LayerBlurParams& blur_params,
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

    int blur_w = 0;
    int blur_h = 0;
    int blur_scratch_w = 0;
    int blur_scratch_h = 0;
    const bool blur_targets_ready =
        scratch_textures.blur_texture &&
        scratch_textures.blur_scratch_texture &&
        scratch_textures.blur_texture != scratch_textures.blur_scratch_texture &&
        query_texture_size(scratch_textures.blur_texture, blur_w, blur_h) &&
        query_texture_size(scratch_textures.blur_scratch_texture, blur_scratch_w, blur_scratch_h) &&
        blur_w == target_w &&
        blur_h == target_h &&
        blur_scratch_w == target_w &&
        blur_scratch_h == target_h;

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);

    const TextureStateSnapshot base_state = capture_texture_state(base_layer_texture);
    const TextureStateSnapshot output_state = capture_texture_state(composited_output_texture);
    TextureStateSnapshot dark_mask_state{};
    TextureStateSnapshot blur_state{};
    TextureStateSnapshot blur_scratch_state{};
    if (scratch_textures.dark_mask_texture) {
        dark_mask_state = capture_texture_state(scratch_textures.dark_mask_texture);
    }
    if (scratch_textures.blur_texture) {
        blur_state = capture_texture_state(scratch_textures.blur_texture);
    }
    if (scratch_textures.blur_scratch_texture) {
        blur_scratch_state = capture_texture_state(scratch_textures.blur_scratch_texture);
    }

    auto restore_state_and_target = [&]() {
        restore_texture_state(base_layer_texture, base_state);
        restore_texture_state(composited_output_texture, output_state);
        if (scratch_textures.dark_mask_texture) {
            restore_texture_state(scratch_textures.dark_mask_texture, dark_mask_state);
        }
        if (scratch_textures.blur_texture) {
            restore_texture_state(scratch_textures.blur_texture, blur_state);
        }
        if (scratch_textures.blur_scratch_texture) {
            restore_texture_state(scratch_textures.blur_scratch_texture, blur_scratch_state);
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
        const SDL_BlendMode alpha_copy = alpha_copy_blend_mode();
        const SDL_BlendMode alpha_masked_mul = alpha_masked_multiply_blend_mode();
        if (alpha_copy == SDL_BLENDMODE_INVALID) {
            if (!warned_missing_alpha_copy_blend_mode_) {
                vibble::log::warn("[LayerEffectProcessor] Alpha copy blend mode unavailable; skipping dark-mask lighting.");
                warned_missing_alpha_copy_blend_mode_ = true;
            }
        } else if (alpha_masked_mul == SDL_BLENDMODE_INVALID) {
            if (!warned_missing_alpha_masked_multiply_blend_mode_) {
                vibble::log::warn("[LayerEffectProcessor] Alpha-masked multiply blend mode unavailable; skipping dark-mask lighting.");
                warned_missing_alpha_masked_multiply_blend_mode_ = true;
            }
        } else {
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
            if (!lights.empty() && ensure_light_accum_texture(target_w, target_h)) {
                clear_target(renderer_, light_accum_texture_);

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
                        SDL_SetTextureBlendMode(falloff_texture, SDL_BLENDMODE_ADD);
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

                    SDL_SetRenderTarget(renderer_, light_accum_texture_);
                    for (int pass = 0; pass < pass_count; ++pass) {
                        SDL_RenderTexture(renderer_, falloff_texture, nullptr, &light_dst);
                    }
                    drew_light_energy = true;
                }

                if (drew_light_energy) {
                    const SDL_BlendMode add_rgb_preserve_alpha = add_rgb_preserve_alpha_blend_mode();
                    if (add_rgb_preserve_alpha == SDL_BLENDMODE_INVALID) {
                        if (!warned_missing_add_rgb_preserve_alpha_blend_mode_) {
                            vibble::log::warn("[LayerEffectProcessor] RGB-add preserve-alpha blend mode unavailable; skipping light contribution.");
                            warned_missing_add_rgb_preserve_alpha_blend_mode_ = true;
                        }
                    } else {
                        SDL_SetRenderTarget(renderer_, scratch_textures.dark_mask_texture);
                        SDL_SetTextureBlendMode(light_accum_texture_, add_rgb_preserve_alpha);
                        SDL_SetTextureAlphaMod(light_accum_texture_, 255);
                        SDL_SetTextureColorMod(light_accum_texture_, 255, 255, 255);
                        SDL_RenderTexture(renderer_, light_accum_texture_, nullptr, nullptr);
                    }
                }
            }

            SDL_SetRenderTarget(renderer_, composited_output_texture);
            SDL_SetTextureBlendMode(scratch_textures.dark_mask_texture, alpha_masked_mul);
            SDL_SetTextureAlphaMod(scratch_textures.dark_mask_texture, 255);
            SDL_SetTextureColorMod(scratch_textures.dark_mask_texture, 255, 255, 255);
            SDL_RenderTexture(renderer_, scratch_textures.dark_mask_texture, nullptr, nullptr);
            result.lighting_applied = true;
        }
    }

    // Step 5: fog over composited layer only.
    if (fog_params.enabled) {
        SDL_Texture* fog_texture = ensure_fog_band_texture();
        const float fog_bottom = std::clamp(fog_params.bottom_y_px, 0.0f, static_cast<float>(target_h));
        if (fog_texture && fog_bottom > 1.0f) {
            const float depth_t = std::clamp(fog_params.normalized_depth, 0.0f, 1.0f);
            const float shaped = std::pow(depth_t, 0.64f);
            const float density = smoothstep(0.03f, 0.93f, shaped);
            const float fog_strength = std::clamp(
                density * (0.52f + (0.48f * shaped)),
                0.0f,
                1.0f);
            const Uint8 fog_alpha = static_cast<Uint8>(std::clamp(
                static_cast<int>(std::lround(fog_strength * 255.0f)),
                0,
                255));
            if (fog_alpha > 0) {
                SDL_SetRenderTarget(renderer_, composited_output_texture);
                SDL_SetTextureBlendMode(fog_texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureColorMod(fog_texture, fog_params.tint.r, fog_params.tint.g, fog_params.tint.b);
                SDL_SetTextureAlphaMod(fog_texture, fog_alpha);

                const SDL_FRect dst_rect{
                    0.0f,
                    0.0f,
                    static_cast<float>(target_w),
                    fog_bottom
                };
                SDL_RenderTexture(renderer_, fog_texture, nullptr, &dst_rect);

                int fog_w = 0;
                int fog_h = 0;
                if (query_texture_size(fog_texture, fog_w, fog_h)) {
                    const Uint8 detail_alpha = static_cast<Uint8>(std::clamp(
                        static_cast<int>(std::lround(static_cast<float>(fog_alpha) * 0.42f)),
                        0,
                        255));
                    if (detail_alpha > 0) {
                        const float drift = std::fmod((depth_t * 7.0f) + 0.31f, 1.0f);
                        const SDL_FRect src_rect{
                            static_cast<float>(fog_w) * (0.05f + (0.15f * drift)),
                            0.0f,
                            static_cast<float>(fog_w) * 0.85f,
                            static_cast<float>(fog_h)
                        };
                        SDL_SetTextureAlphaMod(fog_texture, detail_alpha);
                        SDL_RenderTexture(renderer_, fog_texture, &src_rect, &dst_rect);
                    }
                }

                SDL_SetTextureAlphaMod(fog_texture, 255);
                SDL_SetTextureColorMod(fog_texture, 255, 255, 255);
                result.fog_applied = true;
            }
        }
    }

    // Step 6: blur composited layer only.
    const bool blur_requested =
        blur_params.enabled &&
        ((blur_params.radius_px > kMinimumUsefulBlurRadiusPx) ||
         (blur_params.radial_radius_px > kMinimumUsefulBlurRadiusPx));
    if (blur_requested && blur_targets_ready) {
        if (apply_lens_blur(composited_output_texture,
                            scratch_textures.blur_texture,
                            scratch_textures.blur_scratch_texture,
                            target_w,
                            target_h,
                            blur_params.radius_px,
                            blur_params.optical_center,
                            blur_params.radial_radius_px,
                            blur_params.quality_scale)) {
            result.final_texture = scratch_textures.blur_texture;
            result.blur_applied = true;
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
    if (fog_band_texture_) {
        SDL_DestroyTexture(fog_band_texture_);
        fog_band_texture_ = nullptr;
    }
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
    alpha_copy_blend_mode_ready_ = false;
    add_rgb_preserve_alpha_blend_mode_ = SDL_BLENDMODE_INVALID;
    add_rgb_preserve_alpha_blend_mode_ready_ = false;
    alpha_masked_multiply_blend_mode_ = SDL_BLENDMODE_INVALID;
    alpha_masked_multiply_blend_mode_ready_ = false;

    warned_missing_alpha_copy_blend_mode_ = false;
    warned_missing_add_rgb_preserve_alpha_blend_mode_ = false;
    warned_missing_alpha_masked_multiply_blend_mode_ = false;
}

SDL_Texture* LayerEffectProcessor::ensure_fog_band_texture() {
    if (!renderer_) {
        return nullptr;
    }
    if (fog_band_texture_) {
        return fog_band_texture_;
    }

    SDL_Texture* texture = SDL_CreateTexture(renderer_,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             kFogBandTextureWidth,
                                             kFogBandTextureHeight);
    if (!texture) {
        return nullptr;
    }

    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(texture, nullptr, &pixels, &pitch) || !pixels || pitch <= 0) {
        SDL_DestroyTexture(texture);
        return nullptr;
    }

    auto* base = static_cast<std::uint8_t*>(pixels);
    for (int y = 0; y < kFogBandTextureHeight; ++y) {
        auto* row = base + (pitch * y);
        const float v = static_cast<float>(y) / static_cast<float>(kFogBandTextureHeight - 1);
        const float vertical_falloff = std::pow(1.0f - v, 1.22f);

        for (int x = 0; x < kFogBandTextureWidth; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(kFogBandTextureWidth - 1);
            const float n0 = 0.5f + 0.5f * std::sin((fx * 19.7f) + (v * 4.3f));
            const float n1 = 0.5f + 0.5f * std::sin((fx * 44.2f) - (v * 9.6f) + 1.6f);
            const float n2 = 0.5f + 0.5f * std::sin(((fx + (v * 0.8f)) * 79.5f) + 3.9f);
            const float n3 = 0.5f + 0.5f * std::sin(((fx * 8.1f) - (v * 13.2f)) + 0.2f);
            const float base_noise = std::clamp(
                (n0 * 0.40f) + (n1 * 0.28f) + (n2 * 0.20f) + (n3 * 0.12f),
                0.0f,
                1.0f);

            const float ridged = 1.0f - std::fabs((base_noise * 2.0f) - 1.0f);
            const float wisps = std::pow(std::clamp(ridged, 0.0f, 1.0f), 1.5f);
            const float alpha_f = std::clamp(
                vertical_falloff * (0.46f + (0.39f * base_noise) + (0.15f * wisps)),
                0.0f,
                1.0f);
            const Uint8 alpha = static_cast<Uint8>(std::clamp(
                static_cast<int>(std::lround(alpha_f * 255.0f)),
                0,
                255));

            auto* p = row + (x * 4);
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
            p[3] = alpha;
        }
    }

    SDL_UnlockTexture(texture);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    fog_band_texture_ = texture;
    return fog_band_texture_;
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
    for (int y = 0; y < kTextureSize; ++y) {
        auto* row = base + (pitch * y);
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

            auto* p = row + (x * 4);
            p[0] = value;
            p[1] = value;
            p[2] = value;
            p[3] = value;
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

SDL_BlendMode LayerEffectProcessor::add_rgb_preserve_alpha_blend_mode() {
    if (add_rgb_preserve_alpha_blend_mode_ready_) {
        return add_rgb_preserve_alpha_blend_mode_;
    }
    add_rgb_preserve_alpha_blend_mode_ready_ = true;
    add_rgb_preserve_alpha_blend_mode_ = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    return add_rgb_preserve_alpha_blend_mode_;
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
