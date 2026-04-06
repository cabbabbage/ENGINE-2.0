#include "rendering/render/layer_effect_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
constexpr float kPi = 3.14159265358979323846f;

static void clear_render_target(SDL_Renderer* renderer, SDL_Texture* target) {
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

static void draw_weighted_sample(SDL_Renderer* renderer,
                                 SDL_Texture* texture,
                                 float src_w,
                                 float src_h,
                                 const SDL_FRect& dst_rect,
                                 float weight) {
    if (!renderer || !texture || weight <= 0.0f) {
        return;
    }

    const int alpha_i = static_cast<int>(std::lround(weight * 255.0f));
    if (alpha_i <= 0) {
        return;
    }

    const Uint8 alpha = static_cast<Uint8>(std::clamp(alpha_i, 0, 255));
    SDL_SetTextureAlphaMod(texture, alpha);

    const SDL_FRect src_rect{0.0f, 0.0f, src_w, src_h};
    SDL_RenderTexture(renderer, texture, &src_rect, &dst_rect);
}

static SDL_FRect scaled_rect_about_point(float base_w,
                                         float base_h,
                                         const SDL_FPoint& center,
                                         float scale_x,
                                         float scale_y,
                                         float offset_x,
                                         float offset_y) {
    const float scaled_w = base_w * scale_x;
    const float scaled_h = base_h * scale_y;
    return SDL_FRect{
        center.x - center.x * scale_x + offset_x,
        center.y - center.y * scale_y + offset_y,
        scaled_w,
        scaled_h};
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

static bool apply_original_alpha_mask(SDL_Renderer* renderer,
                                      SDL_Texture* alpha_source,
                                      SDL_Texture* destination,
                                      int draw_w,
                                      int draw_h) {
    if (!renderer || !alpha_source || !destination || draw_w <= 0 || draw_h <= 0) {
        return false;
    }

    const SDL_BlendMode alpha_replace = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDOPERATION_ADD);

    if (alpha_replace == SDL_BLENDMODE_INVALID) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);

    SDL_BlendMode old_src_blend = SDL_BLENDMODE_BLEND;
    Uint8 old_src_alpha = 255;
    SDL_GetTextureBlendMode(alpha_source, &old_src_blend);
    SDL_GetTextureAlphaMod(alpha_source, &old_src_alpha);

    SDL_SetRenderTarget(renderer, destination);
    SDL_SetTextureBlendMode(alpha_source, alpha_replace);
    SDL_SetTextureAlphaMod(alpha_source, 255);

    const SDL_FRect src_rect{0.0f, 0.0f, static_cast<float>(draw_w), static_cast<float>(draw_h)};
    SDL_RenderTexture(renderer, alpha_source, &src_rect, &src_rect);

    restore_texture_state(alpha_source, old_src_blend, old_src_alpha);
    SDL_SetRenderTarget(renderer, previous_target);
    return true;
}

static void apply_axis_blur_pass(SDL_Renderer* renderer,
                                 SDL_Texture* source,
                                 SDL_Texture* destination,
                                 int draw_w,
                                 int draw_h,
                                 float radius_px,
                                 float dir_x,
                                 float dir_y) {
    if (!renderer || !source || !destination || draw_w <= 0 || draw_h <= 0 || radius_px <= 0.01f) {
        return;
    }

    const int kernel_radius = std::clamp(static_cast<int>(std::ceil(radius_px)), 1, 18);
    const float sigma = std::max(0.9f, radius_px * 0.55f);

    std::vector<float> weights;
    build_gaussian_weights(weights, kernel_radius, sigma);

    clear_render_target(renderer, destination);

    const SDL_BlendMode sum_blend = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);

    SDL_SetTextureBlendMode(source, sum_blend);

    const float src_w = static_cast<float>(draw_w);
    const float src_h = static_cast<float>(draw_h);

    for (int i = -kernel_radius; i <= kernel_radius; ++i) {
        const float w = weights[static_cast<std::size_t>(std::abs(i))];
        const float fx = static_cast<float>(i) * dir_x;
        const float fy = static_cast<float>(i) * dir_y;
        const SDL_FRect dst_rect{fx, fy, src_w, src_h};
        draw_weighted_sample(renderer, source, src_w, src_h, dst_rect, w);
    }
}

static void apply_rotational_smudge_pass(SDL_Renderer* renderer,
                                         SDL_Texture* source,
                                         SDL_Texture* destination,
                                         int draw_w,
                                         int draw_h,
                                         float radius_px,
                                         const SDL_FPoint& optical_center) {
    if (!renderer || !source || !destination || draw_w <= 0 || draw_h <= 0 || radius_px <= 0.01f) {
        return;
    }

    clear_render_target(renderer, destination);

    const SDL_BlendMode sum_blend = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);

    SDL_SetTextureBlendMode(source, sum_blend);

    const float src_w = static_cast<float>(draw_w);
    const float src_h = static_cast<float>(draw_h);
    const float max_dim = static_cast<float>(std::max(draw_w, draw_h));
    const float normalized_radius = std::clamp(radius_px / std::max(1.0f, max_dim), 0.0f, 1.0f);

    const int ring_samples = std::clamp(static_cast<int>(std::ceil(radius_px * 1.4f)), 6, 24);
    const float base_weight = 0.32f;
    const float side_weight_total = 1.0f - base_weight;
    const float per_sample_weight = side_weight_total / static_cast<float>(ring_samples);

    const SDL_FRect base_rect{0.0f, 0.0f, src_w, src_h};
    draw_weighted_sample(renderer, source, src_w, src_h, base_rect, base_weight);

    const float max_scale_delta = std::min(0.22f, normalized_radius * 1.8f);
    const float max_translation = radius_px * 0.55f;

    for (int i = 0; i < ring_samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(ring_samples);
        const float angle = t * 2.0f * kPi;

        const float circle_x = std::cos(angle);
        const float circle_y = std::sin(angle);

        const float radial_push = (0.35f + 0.65f * t) * max_translation;
        const float offset_x = circle_x * radial_push;
        const float offset_y = circle_y * radial_push * 0.8f;

        const float scale_push = (0.25f + 0.75f * t) * max_scale_delta;
        const float scale_x = 1.0f + circle_x * scale_push;
        const float scale_y = 1.0f + circle_y * scale_push * 0.7f;

        const SDL_FRect dst_rect = scaled_rect_about_point(
            src_w,
            src_h,
            optical_center,
            std::max(0.05f, scale_x),
            std::max(0.05f, scale_y),
            offset_x,
            offset_y);

        draw_weighted_sample(renderer, source, src_w, src_h, dst_rect, per_sample_weight);
    }
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

    const float clamped_quality = std::clamp(quality_scale, 0.35f, 1.0f);
    const int process_w = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(target_w) * clamped_quality)),
        1,
        target_w);
    const int process_h = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(target_h) * clamped_quality)),
        1,
        target_h);
    const bool reduced_resolution = (process_w != target_w) || (process_h != target_h);

    SDL_Texture* lowres_base = src;

    if (reduced_resolution) {
        clear_render_target(renderer_, scratch);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);

        const SDL_FRect downsampled_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)};
        SDL_RenderTexture(renderer_, src, nullptr, &downsampled_rect);
        lowres_base = scratch;
    }

    const float clamped_radius = std::max(0.0f, radius_px);
    const float clamped_radial_radius = std::max(0.0f, radial_radius_px);

    const float process_blur_radius = clamped_radius * clamped_quality;
    const float process_radial_radius = clamped_radial_radius * clamped_quality;
    const SDL_FPoint lowres_optical_center{
        optical_center.x * clamped_quality,
        optical_center.y * clamped_quality};

    SDL_Texture* ping = dst;
    SDL_Texture* pong = scratch;
    SDL_Texture* current = lowres_base;

    if (process_blur_radius > 0.01f) {
        apply_axis_blur_pass(renderer_, current, ping, process_w, process_h, process_blur_radius, 1.0f, 0.0f);
        current = ping;

        apply_axis_blur_pass(renderer_, current, pong, process_w, process_h, process_blur_radius, 0.0f, 1.0f);
        current = pong;

        const float diag_radius = process_blur_radius * 0.72f;
        apply_axis_blur_pass(
            renderer_,
            current,
            ping,
            process_w,
            process_h,
            diag_radius,
            0.70710678f,
            0.70710678f);
        current = ping;

        apply_axis_blur_pass(
            renderer_,
            current,
            pong,
            process_w,
            process_h,
            diag_radius,
            0.70710678f,
            -0.70710678f);
        current = pong;
    }

    if (process_radial_radius > 0.01f) {
        apply_rotational_smudge_pass(
            renderer_,
            current,
            ping,
            process_w,
            process_h,
            process_radial_radius,
            lowres_optical_center);
        current = ping;
    }

    clear_render_target(renderer_, dst);
    SDL_SetTextureBlendMode(current, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(current, 255);

    if (reduced_resolution) {
        const SDL_FRect src_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)};
        SDL_RenderTexture(renderer_, current, &src_rect, nullptr);
    } else {
        SDL_RenderTexture(renderer_, current, nullptr, nullptr);
    }

    // Force the final destination alpha back to the original source silhouette.
    // This keeps originally solid regions solid without preserving the original color edges.
    apply_original_alpha_mask(renderer_, src, dst, target_w, target_h);

    restore_texture_state(src, src_old_blend, src_old_alpha);
    restore_texture_state(scratch, scratch_old_blend, scratch_old_alpha);
    restore_texture_state(dst, dst_old_blend, dst_old_alpha);
    SDL_SetRenderTarget(renderer_, previous_target);

    return true;
}