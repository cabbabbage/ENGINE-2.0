#include "rendering/render/layer_effect_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr float kMinimumBlurRadiusEpsilonPx = 1.0e-4f;
constexpr int kMaxGaussianKernelRadius = 12;

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
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
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

    if (sum > 1.0e-6f) {
        for (float& w : weights) {
            w /= sum;
        }
    }
}

static void draw_offset_sample(SDL_Renderer* renderer,
                               SDL_Texture* texture,
                               int draw_w,
                               int draw_h,
                               float offset_x,
                               float offset_y,
                               Uint8 alpha) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || alpha == 0) {
        return;
    }

    SDL_SetTextureAlphaMod(texture, alpha);

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

static void draw_scaled_sample(SDL_Renderer* renderer,
                               SDL_Texture* texture,
                               int draw_w,
                               int draw_h,
                               const SDL_FPoint& optical_center,
                               float scale,
                               Uint8 alpha) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || alpha == 0) {
        return;
    }

    SDL_SetTextureAlphaMod(texture, alpha);

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

static int choose_radial_sample_count(float radial_radius_px) {
    if (radial_radius_px <= 1.5f) {
        return 0;
    }
    if (radial_radius_px <= 4.0f) {
        return 4;
    }
    if (radial_radius_px <= 10.0f) {
        return 6;
    }
    if (radial_radius_px <= 22.0f) {
        return 8;
    }
    if (radial_radius_px <= 40.0f) {
        return 10;
    }
    return 12;
}

static Uint8 clamp_alpha_from_unit(float value) {
    return static_cast<Uint8>(std::clamp(
        static_cast<int>(std::lround(value * 255.0f)),
        0,
        255));
}

static bool apply_axis_blur(SDL_Renderer* renderer,
                            SDL_Texture* src,
                            SDL_Texture* dst,
                            int draw_w,
                            int draw_h,
                            float radius_px,
                            float dir_x,
                            float dir_y,
                            SDL_BlendMode sum_blend,
                            int kernel_radius,
                            Uint8 center_alpha,
                            const std::vector<float>& offsets,
                            const std::vector<Uint8>& alphas) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0) {
        return false;
    }
    if (src == dst) {
        return false;
    }

    clear_target(renderer, dst);

    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    if (radius_px <= kMinimumBlurRadiusEpsilonPx || kernel_radius <= 0) {
        SDL_SetTextureAlphaMod(src, 255);
        SDL_RenderTexture(renderer, src, nullptr, nullptr);
        return true;
    }

    SDL_SetTextureBlendMode(src, sum_blend);

    if (center_alpha > 0) {
        draw_offset_sample(renderer, src, draw_w, draw_h, 0.0f, 0.0f, center_alpha);
    }

    const std::size_t tap_count = std::min(offsets.size(), alphas.size());
    for (std::size_t i = 0; i < tap_count; ++i) {
        const float offset = offsets[i];
        const Uint8 alpha = alphas[i];
        if (alpha == 0 || offset <= 0.0f) {
            continue;
        }

        const float dx = offset * dir_x;
        const float dy = offset * dir_y;
        draw_offset_sample(renderer, src, draw_w, draw_h, dx, dy, alpha);
        draw_offset_sample(renderer, src, draw_w, draw_h, -dx, -dy, alpha);
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

    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    if (radial_radius_px <= kMinimumBlurRadiusEpsilonPx) {
        SDL_SetTextureAlphaMod(src, 255);
        SDL_RenderTexture(renderer, src, nullptr, nullptr);
        return true;
    }

    SDL_SetTextureBlendMode(src, sum_blend);

    const int sample_count = choose_radial_sample_count(radial_radius_px);
    if (sample_count <= 0) {
        SDL_SetTextureAlphaMod(src, 255);
        SDL_RenderTexture(renderer, src, nullptr, nullptr);
        return true;
    }

    const float max_dim = static_cast<float>(std::max(draw_w, draw_h));
    const float normalized_radius = std::clamp(radial_radius_px / std::max(1.0f, max_dim), 0.0f, 1.0f);
    const float max_scale_delta = std::min(0.42f, normalized_radius * 2.2f);

    float total_raw_weight = 1.0f;
    std::vector<float> side_weights(static_cast<std::size_t>(sample_count), 0.0f);
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float w = 1.0f - t * 0.82f;
        side_weights[static_cast<std::size_t>(i)] = std::max(0.0f, w);
        total_raw_weight += side_weights[static_cast<std::size_t>(i)] * 2.0f;
    }

    draw_scaled_sample(renderer,
                       src,
                       draw_w,
                       draw_h,
                       optical_center,
                       1.0f,
                       clamp_alpha_from_unit(1.0f / total_raw_weight));

    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float scale_delta = max_scale_delta * t;
        const float weight = side_weights[static_cast<std::size_t>(i)] / total_raw_weight;
        const Uint8 alpha = clamp_alpha_from_unit(weight);
        if (alpha == 0) {
            continue;
        }

        draw_scaled_sample(renderer,
                           src,
                           draw_w,
                           draw_h,
                           optical_center,
                           std::max(0.10f, 1.0f - scale_delta),
                           alpha);

        draw_scaled_sample(renderer,
                           src,
                           draw_w,
                           draw_h,
                           optical_center,
                           1.0f + scale_delta,
                           alpha);
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

void LayerEffectProcessor::ensure_blur_kernel_cache(int kernel_radius, float sigma) const {
    if (kernel_radius <= 0) {
        blur_kernel_cache_.kernel_radius = 0;
        blur_kernel_cache_.sigma = sigma;
        blur_kernel_cache_.offsets.clear();
        blur_kernel_cache_.alphas.clear();
        blur_kernel_cache_.center_alpha = 255;
        return;
    }

    if (blur_kernel_cache_.kernel_radius == kernel_radius &&
        std::fabs(blur_kernel_cache_.sigma - sigma) <= 1.0e-4f) {
        return;
    }

    blur_kernel_cache_.kernel_radius = kernel_radius;
    blur_kernel_cache_.sigma = sigma;
    blur_kernel_cache_.offsets.clear();
    blur_kernel_cache_.alphas.clear();
    blur_kernel_cache_.center_alpha = 255;

    std::vector<float> weights;
    build_gaussian_weights(weights, kernel_radius, sigma);

    blur_kernel_cache_.center_alpha = clamp_alpha_from_unit(weights[0]);

    for (int i = 1; i <= kernel_radius; i += 2) {
        const float w0 = weights[static_cast<std::size_t>(i)];
        const float w1 = (i + 1 <= kernel_radius) ? weights[static_cast<std::size_t>(i + 1)] : 0.0f;
        const float combined = w0 + w1;
        if (combined <= 1.0e-6f) {
            continue;
        }

        const float offset = (w1 > 1.0e-6f)
            ? (static_cast<float>(i) + (w1 / combined))
            : static_cast<float>(i);

        blur_kernel_cache_.offsets.push_back(offset);
        blur_kernel_cache_.alphas.push_back(clamp_alpha_from_unit(combined));
    }
}

SDL_BlendMode LayerEffectProcessor::sum_blend_mode() const {
    if (sum_blend_mode_ready_) {
        return sum_blend_mode_;
    }
    sum_blend_mode_ready_ = true;
    sum_blend_mode_ = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    return sum_blend_mode_;
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

    const SDL_BlendMode sum_blend = sum_blend_mode();
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

    const float clamped_quality = std::clamp(quality_scale, 0.45f, 1.0f);
    const int process_w = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(target_w) * clamped_quality)),
        1,
        target_w);
    const int process_h = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(target_h) * clamped_quality)),
        1,
        target_h);
    const bool reduced_resolution = (process_w != target_w) || (process_h != target_h);

    const float process_blur_radius = std::max(0.0f, radius_px) * clamped_quality;
    const float process_radial_radius = std::max(0.0f, radial_radius_px) * clamped_quality;
    const SDL_FPoint scaled_optical_center{
        optical_center.x * clamped_quality,
        optical_center.y * clamped_quality
    };

    const int kernel_radius = std::clamp(
        static_cast<int>(std::ceil(process_blur_radius * 0.70f)),
        0,
        kMaxGaussianKernelRadius);
    const float sigma = std::max(kMinimumBlurRadiusEpsilonPx,
                                 std::max(0.75f, process_blur_radius * 0.58f));
    ensure_blur_kernel_cache(kernel_radius, sigma);

    SDL_Texture* current = src;

    SDL_SetTextureScaleMode(src, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(scratch, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(dst, SDL_SCALEMODE_LINEAR);

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

    if (process_blur_radius > kMinimumBlurRadiusEpsilonPx && kernel_radius > 0) {
        SDL_Texture* next = pick_other_temp(current);
        if (!apply_axis_blur(renderer_,
                             current,
                             next,
                             process_w,
                             process_h,
                             process_blur_radius,
                             1.0f,
                             0.0f,
                             sum_blend,
                             blur_kernel_cache_.kernel_radius,
                             blur_kernel_cache_.center_alpha,
                             blur_kernel_cache_.offsets,
                             blur_kernel_cache_.alphas)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            SDL_SetRenderTarget(renderer_, previous_target);
            return false;
        }
        current = next;

        next = pick_other_temp(current);
        if (!apply_axis_blur(renderer_,
                             current,
                             next,
                             process_w,
                             process_h,
                             process_blur_radius,
                             0.0f,
                             1.0f,
                             sum_blend,
                             blur_kernel_cache_.kernel_radius,
                             blur_kernel_cache_.center_alpha,
                             blur_kernel_cache_.offsets,
                             blur_kernel_cache_.alphas)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            SDL_SetRenderTarget(renderer_, previous_target);
            return false;
        }
        current = next;
    }

    if (process_radial_radius > kMinimumBlurRadiusEpsilonPx) {
        SDL_Texture* next = pick_other_temp(current);
        if (!apply_radial_zoom_blur(renderer_,
                                    current,
                                    next,
                                    process_w,
                                    process_h,
                                    process_radial_radius,
                                    scaled_optical_center,
                                    sum_blend)) {
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

    restore_texture_state(src, src_state);
    restore_texture_state(scratch, scratch_state);
    restore_texture_state(dst, dst_state);
    SDL_SetRenderTarget(renderer_, previous_target);
    return true;
}

LayerEffectProcessor::LayerProcessResult LayerEffectProcessor::process_layer(
    SDL_Texture* base_layer_texture,
    SDL_Texture* composited_output_texture) {
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

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);

    const TextureStateSnapshot base_state = capture_texture_state(base_layer_texture);
    const TextureStateSnapshot output_state = capture_texture_state(composited_output_texture);

    auto restore_state_and_target = [&]() {
        restore_texture_state(base_layer_texture, base_state);
        restore_texture_state(composited_output_texture, output_state);
        SDL_SetRenderTarget(renderer_, previous_target);
    };

    clear_target(renderer_, composited_output_texture);
    SDL_SetRenderTarget(renderer_, composited_output_texture);
    SDL_SetTextureBlendMode(base_layer_texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(base_layer_texture, 255);
    SDL_SetTextureColorMod(base_layer_texture, 255, 255, 255);
    SDL_RenderTexture(renderer_, base_layer_texture, nullptr, nullptr);

    restore_state_and_target();
    return result;
}


void LayerEffectProcessor::destroy_owned_resources() {
    blur_kernel_cache_.kernel_radius = -1;
    blur_kernel_cache_.sigma = -1.0f;
    blur_kernel_cache_.offsets.clear();
    blur_kernel_cache_.alphas.clear();
    blur_kernel_cache_.center_alpha = 255;

    sum_blend_mode_ = SDL_BLENDMODE_INVALID;
    sum_blend_mode_ready_ = false;
}
