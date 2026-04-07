#include "rendering/render/layer_effect_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

static void clear_target(SDL_Renderer* renderer, SDL_Texture* target) {
    SDL_SetRenderTarget(renderer, target);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
}

static void restore_texture_state(SDL_Texture* texture,
                                  SDL_BlendMode blend_mode,
                                  Uint8 alpha_mod) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, blend_mode);
    SDL_SetTextureAlphaMod(texture, alpha_mod);
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
    destroy_fog_resources();
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

void LayerEffectProcessor::destroy_fog_resources() {
    // Fog system fully removed; nothing to clean up.
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

    SDL_BlendMode src_old_blend = SDL_BLENDMODE_BLEND;
    SDL_BlendMode scratch_old_blend = SDL_BLENDMODE_BLEND;
    SDL_BlendMode dst_old_blend = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(src, &src_old_blend);
    SDL_GetTextureBlendMode(scratch, &scratch_old_blend);
    SDL_GetTextureBlendMode(dst, &dst_old_blend);

    Uint8 src_old_alpha = 255;
    Uint8 scratch_old_alpha = 255;
    Uint8 dst_old_alpha = 255;
    SDL_GetTextureAlphaMod(src, &src_old_alpha);
    SDL_GetTextureAlphaMod(scratch, &scratch_old_alpha);
    SDL_GetTextureAlphaMod(dst, &dst_old_alpha);

    const SDL_BlendMode sum_blend = make_sum_blend_mode();
    if (sum_blend == SDL_BLENDMODE_INVALID) {
        SDL_SetRenderTarget(renderer_, dst);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);
        SDL_RenderTexture(renderer_, src, nullptr, nullptr);

        restore_texture_state(src, src_old_blend, src_old_alpha);
        restore_texture_state(scratch, scratch_old_blend, scratch_old_alpha);
        restore_texture_state(dst, dst_old_blend, dst_old_alpha);
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

    if (process_blur_radius > 0.01f) {
        SDL_Texture* next = pick_other_temp(current);
        if (!apply_axis_blur(renderer_, current, next, process_w, process_h, process_blur_radius, 1.0f, 0.0f, sum_blend)) {
            restore_texture_state(src, src_old_blend, src_old_alpha);
            restore_texture_state(scratch, scratch_old_blend, scratch_old_alpha);
            restore_texture_state(dst, dst_old_blend, dst_old_alpha);
            SDL_SetRenderTarget(renderer_, previous_target);
            return false;
        }
        current = next;

        next = pick_other_temp(current);
        if (!apply_axis_blur(renderer_, current, next, process_w, process_h, process_blur_radius, 0.0f, 1.0f, sum_blend)) {
            restore_texture_state(src, src_old_blend, src_old_alpha);
            restore_texture_state(scratch, scratch_old_blend, scratch_old_alpha);
            restore_texture_state(dst, dst_old_blend, dst_old_alpha);
            SDL_SetRenderTarget(renderer_, previous_target);
            return false;
        }
        current = next;

        const float diagonal_radius = process_blur_radius * 0.55f;
        if (diagonal_radius > 0.75f) {
            next = pick_other_temp(current);
            if (!apply_axis_blur(renderer_, current, next, process_w, process_h, diagonal_radius, 0.70710678f, 0.70710678f, sum_blend)) {
                restore_texture_state(src, src_old_blend, src_old_alpha);
                restore_texture_state(scratch, scratch_old_blend, scratch_old_alpha);
                restore_texture_state(dst, dst_old_blend, dst_old_alpha);
                SDL_SetRenderTarget(renderer_, previous_target);
                return false;
            }
            current = next;

            next = pick_other_temp(current);
            if (!apply_axis_blur(renderer_, current, next, process_w, process_h, diagonal_radius, 0.70710678f, -0.70710678f, sum_blend)) {
                restore_texture_state(src, src_old_blend, src_old_alpha);
                restore_texture_state(scratch, scratch_old_blend, scratch_old_alpha);
                restore_texture_state(dst, dst_old_blend, dst_old_alpha);
                SDL_SetRenderTarget(renderer_, previous_target);
                return false;
            }
            current = next;
        }
    }

    if (process_radial_radius > 0.01f) {
        SDL_Texture* next = pick_other_temp(current);
        if (!apply_radial_zoom_blur(renderer_, current, next, process_w, process_h, process_radial_radius, scaled_optical_center, sum_blend)) {
            restore_texture_state(src, src_old_blend, src_old_alpha);
            restore_texture_state(scratch, scratch_old_blend, scratch_old_alpha);
            restore_texture_state(dst, dst_old_blend, dst_old_alpha);
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
        SDL_RenderTexture(renderer_, current, nullptr, nullptr);
    }

    if (has_blur) {
        const float blur_strength = std::clamp(
            (process_blur_radius + process_radial_radius) / 24.0f,
            0.0f,
            1.0f
        );
        const float reinforce_alpha_f = 0.18f + (0.12f * blur_strength);
        const Uint8 reinforce_alpha = static_cast<Uint8>(
            std::clamp(static_cast<int>(std::lround(reinforce_alpha_f * 255.0f)), 0, 255)
        );

        SDL_SetRenderTarget(renderer_, dst);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, reinforce_alpha);
        SDL_RenderTexture(renderer_, src, nullptr, nullptr);
    }

    restore_texture_state(src, src_old_blend, src_old_alpha);
    restore_texture_state(scratch, scratch_old_blend, scratch_old_alpha);
    restore_texture_state(dst, dst_old_blend, dst_old_alpha);
    SDL_SetRenderTarget(renderer_, previous_target);
    return true;
}