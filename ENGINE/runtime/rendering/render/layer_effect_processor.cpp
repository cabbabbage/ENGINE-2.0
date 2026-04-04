#include "rendering/render/layer_effect_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr int kFogNoiseTileSize = 128;

inline std::uint32_t hash_u32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline float random_01_periodic(int x, int y, int period) {
    if (period <= 0) {
        return 0.0f;
    }
    const int wrapped_x = ((x % period) + period) % period;
    const int wrapped_y = ((y % period) + period) % period;
    const std::uint32_t seed = static_cast<std::uint32_t>(wrapped_x) * 0x9e3779b9U ^
                               static_cast<std::uint32_t>(wrapped_y) * 0x85ebca6bU;
    const std::uint32_t h = hash_u32(seed);
    return static_cast<float>(h & 0x00ffffffU) / static_cast<float>(0x01000000U);
}

inline float smoothstep(float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float value_noise_periodic(float x, float y, int period) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const float tx = smoothstep(x - static_cast<float>(x0));
    const float ty = smoothstep(y - static_cast<float>(y0));
    const float v00 = random_01_periodic(x0, y0, period);
    const float v10 = random_01_periodic(x1, y0, period);
    const float v01 = random_01_periodic(x0, y1, period);
    const float v11 = random_01_periodic(x1, y1, period);
    const float a = lerp(v00, v10, tx);
    const float b = lerp(v01, v11, tx);
    return lerp(a, b, ty);
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

double LayerEffectProcessor::fog_alpha_from_depth(double layer_depth,
                                                  double max_cull_depth,
                                                  double fog_density,
                                                  double fog_depth_curve) {
    const double safe_max_depth = std::max(1.0, max_cull_depth);
    const double safe_density = std::max(0.0, fog_density);
    if (safe_density <= 0.0) {
        return 0.0;
    }
    const double safe_curve = std::clamp(fog_depth_curve, 0.01, 16.0);
    const double depth_norm = std::clamp(std::fabs(layer_depth) / safe_max_depth, 0.0, 1.0);
    const double transmittance = std::exp(-safe_density * std::pow(depth_norm, safe_curve));
    return std::clamp(1.0 - transmittance, 0.0, 1.0);
}

bool LayerEffectProcessor::ensure_fog_noise_tile() const {
    if (!renderer_) {
        return false;
    }
    if (fog_noise_tile_) {
        return true;
    }

    fog_noise_pixels_.assign(static_cast<std::size_t>(kFogNoiseTileSize * kFogNoiseTileSize * 4), 0);
    for (int y = 0; y < kFogNoiseTileSize; ++y) {
        for (int x = 0; x < kFogNoiseTileSize; ++x) {
            const float nx = static_cast<float>(x) / static_cast<float>(kFogNoiseTileSize);
            const float ny = static_cast<float>(y) / static_cast<float>(kFogNoiseTileSize);
            float amplitude = 0.65f;
            float frequency = 1.0f;
            float sum = 0.0f;
            float amp_sum = 0.0f;
            for (int octave = 0; octave < 4; ++octave) {
                const float sample_x = nx * static_cast<float>(kFogNoiseTileSize) * frequency;
                const float sample_y = ny * static_cast<float>(kFogNoiseTileSize) * frequency;
                sum += value_noise_periodic(sample_x, sample_y, kFogNoiseTileSize) * amplitude;
                amp_sum += amplitude;
                amplitude *= 0.5f;
                frequency *= 2.0f;
            }
            float value = (amp_sum > 1e-5f) ? (sum / amp_sum) : 0.0f;
            value = std::clamp(value, 0.0f, 1.0f);
            value = std::pow(value, 1.25f);
            const float softened = std::clamp(0.2f + value * 0.8f, 0.0f, 1.0f);
            const Uint8 channel = static_cast<Uint8>(std::lround(softened * 255.0f));
            const std::size_t base = static_cast<std::size_t>((y * kFogNoiseTileSize + x) * 4);
            fog_noise_pixels_[base + 0] = channel;
            fog_noise_pixels_[base + 1] = channel;
            fog_noise_pixels_[base + 2] = channel;
            fog_noise_pixels_[base + 3] = channel;
        }
    }

    fog_noise_tile_ = SDL_CreateTexture(renderer_,
                                        SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_STATIC,
                                        kFogNoiseTileSize,
                                        kFogNoiseTileSize);
    if (!fog_noise_tile_) {
        fog_noise_pixels_.clear();
        return false;
    }
    SDL_SetTextureBlendMode(fog_noise_tile_, SDL_BLENDMODE_BLEND);
    if (!SDL_UpdateTexture(fog_noise_tile_,
                           nullptr,
                           fog_noise_pixels_.data(),
                           kFogNoiseTileSize * static_cast<int>(sizeof(Uint32)))) {
        SDL_DestroyTexture(fog_noise_tile_);
        fog_noise_tile_ = nullptr;
        fog_noise_pixels_.clear();
        return false;
    }
    return true;
}

bool LayerEffectProcessor::ensure_fog_sheet(int target_w, int target_h) const {
    if (!renderer_ || target_w <= 0 || target_h <= 0) {
        return false;
    }
    if (fog_sheet_tex_ && fog_sheet_w_ == target_w && fog_sheet_h_ == target_h) {
        return true;
    }
    if (fog_sheet_tex_) {
        SDL_DestroyTexture(fog_sheet_tex_);
        fog_sheet_tex_ = nullptr;
    }
    fog_sheet_tex_ = SDL_CreateTexture(renderer_,
                                       SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET,
                                       target_w,
                                       target_h);
    if (!fog_sheet_tex_) {
        fog_sheet_w_ = 0;
        fog_sheet_h_ = 0;
        return false;
    }
    fog_sheet_w_ = target_w;
    fog_sheet_h_ = target_h;
    fog_sheet_last_time_ = -1.0f;
    SDL_SetTextureBlendMode(fog_sheet_tex_, SDL_BLENDMODE_BLEND);
    return true;
}

bool LayerEffectProcessor::update_fog_sheet(int target_w, int target_h, float time_seconds) const {
    if (!ensure_fog_noise_tile() || !ensure_fog_sheet(target_w, target_h)) {
        return false;
    }
    if (fog_sheet_last_time_ >= 0.0f && std::fabs(time_seconds - fog_sheet_last_time_) < 1e-4f) {
        return true;
    }
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    SDL_BlendMode old_noise_blend = SDL_BLENDMODE_BLEND;
    Uint8 old_noise_alpha = 255;
    SDL_GetTextureBlendMode(fog_noise_tile_, &old_noise_blend);
    SDL_GetTextureAlphaMod(fog_noise_tile_, &old_noise_alpha);

    SDL_SetRenderTarget(renderer_, fog_sheet_tex_);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);

    const float drift_t = std::max(0.0f, time_seconds);
    const int tile_a = kFogNoiseTileSize;
    const int tile_b = static_cast<int>(std::lround(static_cast<float>(kFogNoiseTileSize) * 1.7f));
    const int offset_ax = static_cast<int>(std::lround(drift_t * 8.0f)) % tile_a;
    const int offset_ay = static_cast<int>(std::lround(drift_t * 3.0f)) % tile_a;
    const int offset_bx = static_cast<int>(std::lround(drift_t * -5.0f)) % tile_b;
    const int offset_by = static_cast<int>(std::lround(drift_t * 4.0f)) % tile_b;

    auto draw_tiled = [&](int tile_size, int offset_x, int offset_y, Uint8 alpha) {
        if (tile_size <= 0) {
            return;
        }
        SDL_SetTextureBlendMode(fog_noise_tile_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(fog_noise_tile_, alpha);
        for (int y = -tile_size; y < target_h + tile_size; y += tile_size) {
            for (int x = -tile_size; x < target_w + tile_size; x += tile_size) {
                SDL_FRect dst_rect{
                    static_cast<float>(x + offset_x),
                    static_cast<float>(y + offset_y),
                    static_cast<float>(tile_size),
                    static_cast<float>(tile_size)
                };
                SDL_RenderTexture(renderer_, fog_noise_tile_, nullptr, &dst_rect);
            }
        }
    };

    draw_tiled(tile_a, offset_ax, offset_ay, 170);
    draw_tiled(tile_b, offset_bx, offset_by, 95);

    SDL_SetTextureBlendMode(fog_noise_tile_, old_noise_blend);
    SDL_SetTextureAlphaMod(fog_noise_tile_, old_noise_alpha);
    SDL_SetRenderTarget(renderer_, previous_target);
    fog_sheet_last_time_ = time_seconds;
    return true;
}

void LayerEffectProcessor::destroy_fog_resources() {
    if (fog_noise_tile_) {
        SDL_DestroyTexture(fog_noise_tile_);
        fog_noise_tile_ = nullptr;
    }
    if (fog_sheet_tex_) {
        SDL_DestroyTexture(fog_sheet_tex_);
        fog_sheet_tex_ = nullptr;
    }
    fog_noise_pixels_.clear();
    fog_sheet_w_ = 0;
    fog_sheet_h_ = 0;
    fog_sheet_last_time_ = -1.0f;
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
    auto draw_weighted_sample = [&](SDL_Texture* tex,
                                    float offset_x,
                                    float offset_y,
                                    float weight,
                                    int draw_w,
                                    int draw_h) {
        if (weight <= 0.0f) {
            return;
        }
        const int alpha_i = static_cast<int>(std::lround(weight * 255.0f));
        if (alpha_i <= 0) {
            return;
        }
        const Uint8 alpha = static_cast<Uint8>(std::clamp(alpha_i, 0, 255));
        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_FRect src_rect{
            0.0f,
            0.0f,
            static_cast<float>(draw_w),
            static_cast<float>(draw_h)
        };
        SDL_FRect dst_rect{
            offset_x,
            offset_y,
            static_cast<float>(draw_w),
            static_cast<float>(draw_h)
        };
        SDL_RenderTexture(renderer_, tex, &src_rect, &dst_rect);
    };

    const float clamped_quality = std::clamp(quality_scale, 0.35f, 1.0f);
    const int process_w = std::clamp(static_cast<int>(std::lround(static_cast<float>(target_w) * clamped_quality)), 1, target_w);
    const int process_h = std::clamp(static_cast<int>(std::lround(static_cast<float>(target_h) * clamped_quality)), 1, target_h);
    const bool reduced_resolution = process_w != target_w || process_h != target_h;

    SDL_Texture* base_lowres_tex = src;
    if (reduced_resolution) {
        clear_target(scratch);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);
        SDL_FRect downsampled_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };
        SDL_RenderTexture(renderer_, src, nullptr, &downsampled_rect);
        base_lowres_tex = scratch;
    }

    const float clamped_radius = std::max(0.0f, radius_px);
    SDL_Texture* lowres_result_tex = base_lowres_tex;
    const float process_blur_radius = clamped_radius * clamped_quality;
    if (process_blur_radius <= 0.01f) {
        lowres_result_tex = base_lowres_tex;
    } else {
        const int kernel = std::clamp(static_cast<int>(std::ceil(process_blur_radius)), 1, 10);
        const float sigma = std::max(0.75f, process_blur_radius * 0.6f);
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

        clear_target(dst);
        SDL_SetTextureBlendMode(base_lowres_tex, sum_blend);
        for (int i = -kernel; i <= kernel; ++i) {
            const float w = weights[static_cast<std::size_t>(std::abs(i))];
            draw_weighted_sample(base_lowres_tex, static_cast<float>(i), 0.0f, w, process_w, process_h);
        }

        clear_target(scratch);
        SDL_SetTextureBlendMode(dst, sum_blend);
        for (int i = -kernel; i <= kernel; ++i) {
            const float w = weights[static_cast<std::size_t>(std::abs(i))];
            draw_weighted_sample(dst, 0.0f, static_cast<float>(i), w, process_w, process_h);
        }
    }

    if (process_blur_radius > 0.01f) {
        lowres_result_tex = scratch;
    }
    const float clamped_radial_radius = std::max(0.0f, radial_radius_px);
    const float process_radial_radius = clamped_radial_radius * clamped_quality;
    const float max_dimension = static_cast<float>(std::max(process_w, process_h));
    if (process_radial_radius > 0.01f && max_dimension > 1.0f) {
        clear_target(dst);
        SDL_SetTextureBlendMode(scratch, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(scratch, 255);
        SDL_FRect src_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };
        SDL_RenderTexture(renderer_, scratch, &src_rect, &src_rect);

        clear_target(scratch);
        SDL_SetTextureBlendMode(dst, sum_blend);

        const int radial_steps = std::clamp(static_cast<int>(std::ceil(process_radial_radius * 1.2f)), 1, 12);
        const float base_weight = 0.58f;
        const float side_total_weight = std::max(0.0f, 1.0f - base_weight);
        const float side_weight = side_total_weight / static_cast<float>(radial_steps * 2);
        const float max_scale_delta = std::min(0.16f, (process_radial_radius / max_dimension) * 2.4f);
        const SDL_FPoint lowres_optical_center{
            optical_center.x * clamped_quality,
            optical_center.y * clamped_quality
        };

        draw_weighted_sample(dst, 0.0f, 0.0f, base_weight, process_w, process_h);
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
                SDL_SetTextureAlphaMod(dst, alpha);
                SDL_FRect src_rect{
                    0.0f,
                    0.0f,
                    static_cast<float>(process_w),
                    static_cast<float>(process_h)
                };
                SDL_FRect dst_rect{
                    lowres_optical_center.x - lowres_optical_center.x * scale,
                    lowres_optical_center.y - lowres_optical_center.y * scale,
                    static_cast<float>(process_w) * scale,
                    static_cast<float>(process_h) * scale
                };
                SDL_RenderTexture(renderer_, dst, &src_rect, &dst_rect);
            }
        }
        lowres_result_tex = scratch;
    }

    clear_target(dst);
    SDL_SetTextureBlendMode(lowres_result_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(lowres_result_tex, 255);
    if (reduced_resolution) {
        SDL_FRect src_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };
        SDL_RenderTexture(renderer_, lowres_result_tex, &src_rect, nullptr);
    } else {
        SDL_RenderTexture(renderer_, lowres_result_tex, nullptr, nullptr);
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

bool LayerEffectProcessor::apply_atmospheric_fog(SDL_Texture* src,
                                                 SDL_Texture* dst,
                                                 SDL_Texture* scratch,
                                                 int target_w,
                                                 int target_h,
                                                 SDL_Color fog_color,
                                                 float fog_alpha,
                                                 float time_seconds,
                                                 float quality_scale) const {
    if (!renderer_ || !src || !dst || !scratch || target_w <= 0 || target_h <= 0) {
        return false;
    }
    if (src == dst || src == scratch || dst == scratch) {
        return false;
    }
    const float clamped_fog_alpha = std::clamp(fog_alpha, 0.0f, 1.0f);
    if (clamped_fog_alpha <= 0.0f) {
        SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
        SDL_BlendMode src_old_blend = SDL_BLENDMODE_BLEND;
        Uint8 src_old_alpha = 255;
        SDL_GetTextureBlendMode(src, &src_old_blend);
        SDL_GetTextureAlphaMod(src, &src_old_alpha);
        SDL_SetRenderTarget(renderer_, dst);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, 255);
        SDL_RenderTexture(renderer_, src, nullptr, nullptr);
        SDL_SetTextureBlendMode(src, src_old_blend);
        SDL_SetTextureAlphaMod(src, src_old_alpha);
        SDL_SetRenderTarget(renderer_, previous_target);
        return true;
    }
    if (!update_fog_sheet(target_w, target_h, time_seconds)) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    SDL_BlendMode src_old_blend = SDL_BLENDMODE_BLEND;
    SDL_BlendMode dst_old_blend = SDL_BLENDMODE_BLEND;
    SDL_BlendMode scratch_old_blend = SDL_BLENDMODE_BLEND;
    SDL_BlendMode fog_old_blend = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(src, &src_old_blend);
    SDL_GetTextureBlendMode(dst, &dst_old_blend);
    SDL_GetTextureBlendMode(scratch, &scratch_old_blend);
    SDL_GetTextureBlendMode(fog_sheet_tex_, &fog_old_blend);
    Uint8 src_old_alpha = 255;
    Uint8 dst_old_alpha = 255;
    Uint8 scratch_old_alpha = 255;
    Uint8 fog_old_alpha = 255;
    Uint8 fog_old_r = 255;
    Uint8 fog_old_g = 255;
    Uint8 fog_old_b = 255;
    SDL_GetTextureAlphaMod(src, &src_old_alpha);
    SDL_GetTextureAlphaMod(dst, &dst_old_alpha);
    SDL_GetTextureAlphaMod(scratch, &scratch_old_alpha);
    SDL_GetTextureAlphaMod(fog_sheet_tex_, &fog_old_alpha);
    SDL_GetTextureColorMod(fog_sheet_tex_, &fog_old_r, &fog_old_g, &fog_old_b);

    auto clear_target = [&](SDL_Texture* target) {
        SDL_SetRenderTarget(renderer_, target);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
    };

    // True source-over fog: affects both color and alpha so transparent layer regions
    // can still carry atmospheric haze and occlude farther layers.
    const SDL_BlendMode fog_blend = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD);

    const float clamped_quality = std::clamp(quality_scale, 0.35f, 1.0f);
    const int process_w = std::clamp(static_cast<int>(std::lround(static_cast<float>(target_w) * clamped_quality)), 1, target_w);
    const int process_h = std::clamp(static_cast<int>(std::lround(static_cast<float>(target_h) * clamped_quality)), 1, target_h);
    const bool reduced_resolution = process_w != target_w || process_h != target_h;

    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);
    if (reduced_resolution) {
        clear_target(scratch);
        SDL_FRect downsample_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };
        SDL_RenderTexture(renderer_, src, nullptr, &downsample_rect);
    } else {
        clear_target(dst);
        SDL_RenderTexture(renderer_, src, nullptr, nullptr);
    }

    if (clamped_fog_alpha > 0.0f) {
        const int fog_alpha_i = static_cast<int>(std::lround(clamped_fog_alpha * 255.0f));
        const Uint8 fog_alpha_u8 = static_cast<Uint8>(std::clamp(fog_alpha_i, 0, 255));
        const SDL_BlendMode active_fog_blend = (fog_blend == SDL_BLENDMODE_INVALID)
            ? SDL_BLENDMODE_BLEND
            : fog_blend;
        SDL_SetTextureBlendMode(fog_sheet_tex_, active_fog_blend);
        SDL_SetTextureColorMod(fog_sheet_tex_, fog_color.r, fog_color.g, fog_color.b);
        SDL_SetTextureAlphaMod(fog_sheet_tex_, fog_alpha_u8);
        SDL_SetRenderTarget(renderer_, reduced_resolution ? scratch : dst);
        SDL_FRect src_rect{
            0.0f,
            0.0f,
            static_cast<float>(target_w),
            static_cast<float>(target_h)
        };
        SDL_FRect dst_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };
        SDL_RenderTexture(renderer_, fog_sheet_tex_, &src_rect, &dst_rect);
    }

    if (reduced_resolution) {
        clear_target(dst);
        SDL_SetTextureBlendMode(scratch, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(scratch, 255);
        SDL_FRect src_rect{
            0.0f,
            0.0f,
            static_cast<float>(process_w),
            static_cast<float>(process_h)
        };
        SDL_RenderTexture(renderer_, scratch, &src_rect, nullptr);
    }

    SDL_SetTextureBlendMode(src, src_old_blend);
    SDL_SetTextureBlendMode(dst, dst_old_blend);
    SDL_SetTextureBlendMode(scratch, scratch_old_blend);
    SDL_SetTextureBlendMode(fog_sheet_tex_, fog_old_blend);
    SDL_SetTextureAlphaMod(src, src_old_alpha);
    SDL_SetTextureAlphaMod(dst, dst_old_alpha);
    SDL_SetTextureAlphaMod(scratch, scratch_old_alpha);
    SDL_SetTextureAlphaMod(fog_sheet_tex_, fog_old_alpha);
    SDL_SetTextureColorMod(fog_sheet_tex_, fog_old_r, fog_old_g, fog_old_b);
    SDL_SetRenderTarget(renderer_, previous_target);
    return true;
}
