#include "rendering/render/layer_effect_processor.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

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
                                           float radial_radius_px) const {
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

    // Accumulate weighted taps with alpha-aware color accumulation.
    // Color uses SRC_ALPHA so transparent texels do not inject RGB energy and blow out to white.
    // Alpha uses ONE so coverage sums linearly (not squared by SRC_ALPHA again).
    const SDL_BlendMode sum_blend = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    if (sum_blend == SDL_BLENDMODE_INVALID) {
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);
        SDL_SetRenderTarget(renderer_, dst);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        SDL_RenderTexture(renderer_, src, nullptr, nullptr);
        SDL_SetTextureBlendMode(src, src_old_blend);
        SDL_SetTextureAlphaMod(src, src_old_alpha);
        SDL_SetTextureBlendMode(scratch, scratch_old_blend);
        SDL_SetTextureAlphaMod(scratch, scratch_old_alpha);
        SDL_SetTextureBlendMode(dst, dst_old_blend);
        SDL_SetTextureAlphaMod(dst, dst_old_alpha);
        SDL_SetRenderTarget(renderer_, previous_target);
        return true;
    }

    auto clear_target = [&](SDL_Texture* target) {
        SDL_SetRenderTarget(renderer_, target);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
    };
    auto draw_weighted_sample = [&](SDL_Texture* tex, float offset_x, float offset_y, float weight) {
        if (weight <= 0.0f) {
            return;
        }
        const int alpha_i = static_cast<int>(std::lround(weight * 255.0f));
        if (alpha_i <= 0) {
            return;
        }
        const Uint8 alpha = static_cast<Uint8>(std::clamp(alpha_i, 0, 255));
        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_FRect dst_rect{
            offset_x,
            offset_y,
            static_cast<float>(target_w),
            static_cast<float>(target_h)
        };
        SDL_RenderTexture(renderer_, tex, nullptr, &dst_rect);
    };

    const float clamped_radius = std::max(0.0f, radius_px);
    if (clamped_radius <= 0.01f) {
        clear_target(dst);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);
        SDL_RenderTexture(renderer_, src, nullptr, nullptr);
    } else {
        const int kernel = std::clamp(static_cast<int>(std::ceil(clamped_radius)), 1, 10);
        const float sigma = std::max(0.75f, clamped_radius * 0.6f);
        std::vector<float> weights(static_cast<std::size_t>(kernel + 1), 0.0f);
        float weight_sum = 0.0f;
        for (int i = 0; i <= kernel; ++i) {
            const float x = static_cast<float>(i);
            const float w = std::exp(-(x * x) / (2.0f * sigma * sigma));
            weights[static_cast<std::size_t>(i)] = w;
            weight_sum += (i == 0) ? w : (2.0f * w);
        }
        if (weight_sum > 1e-6f) {
            for (float& w : weights) {
                w /= weight_sum;
            }
        }

        clear_target(scratch);
        SDL_SetTextureBlendMode(src, sum_blend);
        for (int i = -kernel; i <= kernel; ++i) {
            const float w = weights[static_cast<std::size_t>(std::abs(i))];
            draw_weighted_sample(src, static_cast<float>(i), 0.0f, w);
        }

        clear_target(dst);
        SDL_SetTextureBlendMode(scratch, sum_blend);
        for (int i = -kernel; i <= kernel; ++i) {
            const float w = weights[static_cast<std::size_t>(std::abs(i))];
            draw_weighted_sample(scratch, 0.0f, static_cast<float>(i), w);
        }
    }

    const float clamped_radial_radius = std::max(0.0f, radial_radius_px);
    const float max_dimension = static_cast<float>(std::max(target_w, target_h));
    if (clamped_radial_radius > 0.01f && max_dimension > 1.0f) {
        clear_target(scratch);
        SDL_SetTextureBlendMode(dst, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(dst, 255);
        SDL_RenderTexture(renderer_, dst, nullptr, nullptr);

        clear_target(dst);
        SDL_SetTextureBlendMode(scratch, sum_blend);

        const int radial_steps = std::clamp(static_cast<int>(std::ceil(clamped_radial_radius * 1.2f)), 1, 12);
        const float base_weight = 0.58f;
        const float side_total_weight = std::max(0.0f, 1.0f - base_weight);
        const float side_weight = side_total_weight / static_cast<float>(radial_steps * 2);
        const float max_scale_delta = std::min(0.16f, (clamped_radial_radius / max_dimension) * 2.4f);

        draw_weighted_sample(scratch, 0.0f, 0.0f, base_weight);
        for (int step = 1; step <= radial_steps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(radial_steps + 1);
            const float scale_delta = max_scale_delta * t;
            for (int sign = -1; sign <= 1; sign += 2) {
                const float scale = 1.0f + static_cast<float>(sign) * scale_delta;
                if (scale <= 0.05f) {
                    continue;
                }
                const int alpha_i = static_cast<int>(std::lround(side_weight * 255.0f));
                if (alpha_i <= 0) {
                    continue;
                }
                const Uint8 alpha = static_cast<Uint8>(std::clamp(alpha_i, 0, 255));
                SDL_SetTextureAlphaMod(scratch, alpha);
                SDL_FRect dst_rect{
                    optical_center.x - optical_center.x * scale,
                    optical_center.y - optical_center.y * scale,
                    static_cast<float>(target_w) * scale,
                    static_cast<float>(target_h) * scale
                };
                SDL_RenderTexture(renderer_, scratch, nullptr, &dst_rect);
            }
        }
    }

    SDL_SetTextureBlendMode(src, src_old_blend);
    SDL_SetTextureAlphaMod(src, src_old_alpha);
    SDL_SetTextureBlendMode(scratch, scratch_old_blend);
    SDL_SetTextureAlphaMod(scratch, scratch_old_alpha);
    SDL_SetTextureBlendMode(dst, dst_old_blend);
    SDL_SetTextureAlphaMod(dst, dst_old_alpha);
    SDL_SetRenderTarget(renderer_, previous_target);
    return true;
}
