#include "rendering/render/layer_effect_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "utils/log.hpp"

namespace {

constexpr float kMinimumBlurRadiusEpsilonPx = 1.0e-4f;

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
    const int sample_count = std::clamp(static_cast<int>(std::ceil(radial_radius_px * 2.0f)), 2, 64);
    const float max_scale_delta = std::min(0.65f, normalized_radius * 3.0f);

    const float base_raw_weight = 1.0f;
    float total_raw_weight = base_raw_weight;

    std::vector<float> side_weights(static_cast<std::size_t>(sample_count), 0.0f);
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float w = 1.0f - t;
        side_weights[static_cast<std::size_t>(i)] = w;
        total_raw_weight += (2.0f * w);
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
        const float raw_weight = side_weights[static_cast<std::size_t>(i)];
        const float symmetric_weight = raw_weight / total_raw_weight;

        draw_weighted_scaled_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            optical_center,
            std::max(0.05f, 1.0f - scale_delta),
            symmetric_weight
        );

        draw_weighted_scaled_sample(
            renderer,
            src,
            draw_w,
            draw_h,
            optical_center,
            1.0f + scale_delta,
            symmetric_weight
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
                    const float effective_intensity = std::max(0.0f, light.intensity);
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

    (void)layer_depth_min;
    (void)layer_depth_max;
    (void)fog_params;

    restore_state_and_target();
    return result;
}

void LayerEffectProcessor::destroy_owned_resources() {
    destroy_lighting_resources();
}

void LayerEffectProcessor::destroy_lighting_resources() {
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
