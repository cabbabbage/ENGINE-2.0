#include "rendering/render/dof_blur_chain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "rendering/render/render_diagnostics.hpp"

namespace {

constexpr float kEffectEpsilon = 1.0e-4f;

constexpr float kZoomBlurScaleMultiplier = 0.80f;

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

int ping_pong_frame_index(int frame_index, int frame_count) {
    if (frame_count <= 1) {
        return 0;
    }

    const int cycle = frame_count * 2 - 2;
    if (cycle <= 0) {
        return 0;
    }

    int wrapped = frame_index % cycle;
    if (wrapped < 0) {
        wrapped += cycle;
    }

    if (wrapped >= frame_count) {
        wrapped = cycle - wrapped;
    }

    return std::clamp(wrapped, 0, frame_count - 1);
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

float dust_depth_curve(float world_distance, float max_world_distance) {
    if (max_world_distance <= 0.0f) {
        return 0.0f;
    }

    const float t = std::clamp(world_distance / max_world_distance, 0.0f, 1.0f);
    return std::pow(smoothstep01(t), dof_blur_chain::atmospheric_dust_tuning::kDepthRampPower);
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

SDL_FPoint resolve_layer_dust_anchor(const dof_blur_chain::LayerTexture& layer, int width, int height) {
    const float fallback_x = static_cast<float>(std::max(1, width)) * 0.5f;
    const float fallback_y = static_cast<float>(std::max(1, height));

    if (!layer.has_dust_bottom_center ||
        !std::isfinite(layer.dust_bottom_center.x) ||
        !std::isfinite(layer.dust_bottom_center.y)) {
        return SDL_FPoint{fallback_x, fallback_y};
    }

    return SDL_FPoint{
        std::clamp(layer.dust_bottom_center.x, 0.0f, static_cast<float>(std::max(1, width))),
        std::clamp(layer.dust_bottom_center.y, 0.0f, static_cast<float>(std::max(1, height)))
    };
}

SDL_BlendMode accumulation_blend_mode() {
    static const SDL_BlendMode mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    return mode;
}

void set_accumulation_blend_mode(SDL_Texture* texture) {
    if (!texture) {
        return;
    }

    if (!SDL_SetTextureBlendMode(texture, accumulation_blend_mode())) {
        // Preserve behavior on renderers without custom blend mode support.
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
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

    set_accumulation_blend_mode(src);
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

std::uint32_t hash_u32(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

std::uint32_t hash_tile(int depth_layer, int tile_x, int tile_y, int salt) {
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::uint32_t value) {
        h ^= value;
        h *= 16777619u;
    };

    mix(static_cast<std::uint32_t>(depth_layer * 73856093));
    mix(static_cast<std::uint32_t>(tile_x * 19349663));
    mix(static_cast<std::uint32_t>(tile_y * 83492791));
    mix(static_cast<std::uint32_t>(salt * 2654435761u));

    return hash_u32(h);
}

float hash_unit(int depth_layer, int tile_x, int tile_y, int salt) {
    return static_cast<float>(hash_tile(depth_layer, tile_x, tile_y, salt) & 0xFFFFu) / 65535.0f;
}

SDL_FlipMode tile_flip_mode(int depth_layer, int tile_x, int tile_y) {
    const std::uint32_t h = hash_tile(depth_layer, tile_x, tile_y, 97);
    int flags = SDL_FLIP_NONE;
    if ((h & 1u) != 0u) {
        flags |= SDL_FLIP_HORIZONTAL;
    }
    if ((h & 2u) != 0u) {
        flags |= SDL_FLIP_VERTICAL;
    }
    return static_cast<SDL_FlipMode>(flags);
}

float layer_world_distance(const dof_blur_chain::LayerTexture& layer,
                           int focus_depth_layer,
                           const dof_blur_chain::DustAnchor& dust_anchor) {
    if (layer.world_distance_from_focus > 0.0f) {
        return layer.world_distance_from_focus;
    }

    const int layer_distance = std::abs(layer.depth_layer - focus_depth_layer);
    return static_cast<float>(layer_distance) * std::max(0.001f, dust_anchor.world_units_per_depth_layer);
}

float resolve_dust_tile_scale(const dof_blur_chain::LayerTexture& layer,
                              int focus_depth_layer,
                              float depth_t,
                              bool foreground_layer,
                              bool background_seed) {
    if (background_seed) {
        return dof_blur_chain::atmospheric_dust_tuning::kBackgroundFarTileScale;
    }

    const int layer_distance = std::abs(layer.depth_layer - focus_depth_layer);
    if (layer_distance <= 0) {
        return dof_blur_chain::atmospheric_dust_tuning::kFocusTileScale;
    }

    if (foreground_layer) {
        return lerp_float(dof_blur_chain::atmospheric_dust_tuning::kForegroundNearTileScale,
                          dof_blur_chain::atmospheric_dust_tuning::kForegroundFarTileScale,
                          depth_t);
    }

    return lerp_float(dof_blur_chain::atmospheric_dust_tuning::kBackgroundNearTileScale,
                      dof_blur_chain::atmospheric_dust_tuning::kBackgroundFarTileScale,
                      depth_t);
}

float camera_zoom_to_screen_scale(float camera_zoom_percent) {
    if (!std::isfinite(camera_zoom_percent)) {
        return 1.0f;
    }

    return 1.0f + std::clamp(camera_zoom_percent, 0.0f, 100.0f) * 0.01f;
}

float layer_parallax_factor(int depth_layer, int focus_depth_layer) {
    const int distance = std::abs(depth_layer - focus_depth_layer);
    const float distance_t = std::clamp(static_cast<float>(distance) / 8.0f, 0.0f, 1.0f);
    if (depth_layer > focus_depth_layer) {
        // Background layers move less relative to camera pan.
        return lerp_float(0.85f, 0.35f, distance_t);
    }
    if (depth_layer < focus_depth_layer) {
        // Foreground layers move more relative to camera pan.
        return lerp_float(1.10f, 1.75f, distance_t);
    }
    return 1.0f;
}

SDL_FPoint layer_world_scroll_px(const dof_blur_chain::DustAnchor& dust_anchor,
                                 int depth_layer,
                                 int focus_depth_layer,
                                 float tile_w,
                                 float tile_h) {
    if (tile_w <= 0.0f || tile_h <= 0.0f) {
        return SDL_FPoint{0.0f, 0.0f};
    }

    const float parallax = layer_parallax_factor(depth_layer, focus_depth_layer);
    const float units_per_layer = std::max(0.001f, dust_anchor.world_units_per_depth_layer);

    // Convert world motion to tile-space phase so the dust field stays attached to layer motion.
    const float world_to_tile_x = 1.0f / units_per_layer;
    const float world_to_tile_y = 0.6f / units_per_layer;
    const float tile_phase_x = dust_anchor.world_x * world_to_tile_x * parallax;
    const float tile_phase_y = dust_anchor.world_z * world_to_tile_y * parallax;

    const float scroll_x = std::fmod(tile_phase_x * tile_w, tile_w);
    const float scroll_y = std::fmod(tile_phase_y * tile_h, tile_h);
    return SDL_FPoint{scroll_x, scroll_y};
}

bool draw_bottom_center_anchored_dust_tiles(SDL_Renderer* renderer,
                                            const std::vector<SDL_Texture*>& dust_frames,
                                            int width,
                                            int height,
                                            int depth_layer,
                                            float tile_w,
                                            float tile_h,
                                            SDL_FPoint anchor,
                                            SDL_FPoint world_scroll_px,
                                            float time_seconds) {
    if (!renderer || dust_frames.empty() || width <= 0 || height <= 0 || tile_w <= 0.0f || tile_h <= 0.0f) {
        return true;
    }

    const int frame_count = static_cast<int>(dust_frames.size());
    if (frame_count <= 0) {
        return true;
    }

    // The dust field is locked to the bottom-center of the layer content.
    // Tile (0, 0) has its bottom edge on this anchor. When depth changes the tile size,
    // the field expands/contracts from the layer floor edge instead of swimming around screen center.
    const float anchor_x = std::clamp(anchor.x + world_scroll_px.x, 0.0f, static_cast<float>(width));
    const float anchor_y = std::clamp(anchor.y + world_scroll_px.y, 0.0f, static_cast<float>(height));

    const int tiles_left = static_cast<int>(std::ceil(anchor_x / tile_w)) + 2;
    const int tiles_right = static_cast<int>(std::ceil((static_cast<float>(width) - anchor_x) / tile_w)) + 2;
    const int tiles_up = static_cast<int>(std::ceil(anchor_y / tile_h)) + 2;
    const int tiles_down = static_cast<int>(std::ceil((static_cast<float>(height) - anchor_y) / tile_h)) + 2;

    const int base_frame =
        static_cast<int>(std::floor(std::max(0.0f, time_seconds) *
                                    dof_blur_chain::atmospheric_dust_tuning::kAnimationFps));

    bool ok = true;

    for (int gy = -tiles_up; gy <= tiles_down; ++gy) {
        for (int gx = -tiles_left; gx <= tiles_right; ++gx) {
            const std::uint32_t h = hash_tile(depth_layer, gx, gy, 211);
            const int frame_offset = static_cast<int>(h % static_cast<std::uint32_t>(frame_count));
            const int frame_index = ping_pong_frame_index(base_frame + frame_offset, frame_count);

            SDL_Texture* dust = dust_frames[static_cast<std::size_t>(frame_index)];
            if (!dust) {
                continue;
            }

            const TextureStateSnapshot state = capture_texture_state(dust);

            SDL_SetTextureBlendMode(dust, SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(dust, static_cast<Uint8>(
                std::clamp(static_cast<int>(std::lround(dof_blur_chain::atmospheric_dust_tuning::kDrawAlpha * 255.0f)),
                           0,
                           255)));
            SDL_SetTextureColorMod(dust, 255, 255, 255);

            const SDL_FRect dst{
                anchor_x - tile_w * 0.5f + static_cast<float>(gx) * tile_w,
                anchor_y - tile_h + static_cast<float>(gy) * tile_h,
                tile_w,
                tile_h
            };

            const SDL_FlipMode flip = tile_flip_mode(depth_layer, gx, gy);
            if (!SDL_RenderTextureRotated(renderer, dust, nullptr, &dst, 0.0, nullptr, flip)) {
                ok = false;
            }

            restore_texture_state(dust, state);

            if (!ok) {
                break;
            }
        }

        if (!ok) {
            break;
        }
    }

    return ok;
}

bool add_layer_atmospheric_dust(SDL_Renderer* renderer,
                                SDL_Texture* src,
                                SDL_Texture* dst,
                                const std::vector<SDL_Texture*>& dust_frames,
                                int width,
                                int height,
                                const dof_blur_chain::LayerTexture& layer,
                                int focus_depth_layer,
                                float max_layer_world_distance,
                                float time_seconds,
                                float camera_zoom_percent,
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

    SDL_Texture* first_frame = dust_frames.front();
    if (!first_frame) {
        return true;
    }

    float source_w = 0.0f;
    float source_h = 0.0f;
    if (!SDL_GetTextureSize(first_frame, &source_w, &source_h) || source_w <= 0.0f || source_h <= 0.0f) {
        return true;
    }

    const float world_distance = background_seed
        ? std::max(0.0f, max_layer_world_distance)
        : layer_world_distance(layer, focus_depth_layer, dust_anchor);

    const float cutoff_distance = dust_anchor.max_dust_world_distance > 0.0f
        ? dust_anchor.max_dust_world_distance
        : 0.0f;

    if (!background_seed && cutoff_distance > 0.0f && world_distance > cutoff_distance) {
        return true;
    }

    const float effective_max_distance = cutoff_distance > 0.0f
        ? cutoff_distance
        : std::max(1.0f, max_layer_world_distance);

    const float depth_t = dust_depth_curve(world_distance, effective_max_distance);
    const float tile_scale = resolve_dust_tile_scale(layer,
                                                     focus_depth_layer,
                                                     depth_t,
                                                     foreground_layer,
                                                     background_seed) *
                             camera_zoom_to_screen_scale(camera_zoom_percent);

    const float tile_w = std::max(1.0f, source_w * std::max(0.001f, tile_scale));
    const float tile_h = std::max(1.0f, source_h * std::max(0.001f, tile_scale));

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    render_diagnostics::set_render_target(renderer, dst);

    const bool ok = draw_bottom_center_anchored_dust_tiles(renderer,
                                                    dust_frames,
                                                    width,
                                                    height,
                                                    layer.depth_layer,
                                                    tile_w,
                                                    tile_h,
                                                    background_seed
                                                        ? SDL_FPoint{static_cast<float>(width) * 0.5f,
                                                                     static_cast<float>(height)}
                                                        : resolve_layer_dust_anchor(layer, width, height),
                                                    layer_world_scroll_px(dust_anchor,
                                                                          layer.depth_layer,
                                                                          focus_depth_layer,
                                                                          tile_w,
                                                                          tile_h),
                                                    time_seconds);

    render_diagnostics::set_render_target(renderer, previous_target);
    return ok;
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

bool draw_weighted_texture_region(SDL_Renderer* renderer,
                                  SDL_Texture* texture,
                                  const SDL_FRect& src_rect,
                                  const SDL_FRect& dst_rect,
                                  float weight) {
    if (!renderer || !texture || src_rect.w <= 0.0f || src_rect.h <= 0.0f ||
        dst_rect.w <= 0.0f || dst_rect.h <= 0.0f || weight <= 0.0f) {
        return true;
    }

    const int alpha = std::clamp(static_cast<int>(std::lround(weight * 255.0f)), 0, 255);
    if (alpha <= 0) {
        return true;
    }

    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(alpha));
    return render_diagnostics::render_texture(renderer, texture, &src_rect, &dst_rect);
}

bool apply_edge_lens_warp(SDL_Renderer* renderer,
                          SDL_Texture* src,
                          SDL_Texture* dst,
                          int draw_w,
                          int draw_h,
                          float warp_strength01) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0 || src == dst) {
        return false;
    }

    const float strength = smoothstep01(warp_strength01);
    if (!dof_blur_chain::edge_lens_warp_tuning::kEnabled || strength <= kEffectEpsilon) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);

    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);

    set_accumulation_blend_mode(src);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const float w = static_cast<float>(draw_w);
    const float h = static_cast<float>(draw_h);
    const float max_dim = static_cast<float>(std::max(draw_w, draw_h));
    const float min_dim = static_cast<float>(std::min(draw_w, draw_h));

    const int samples = std::clamp(
        dof_blur_chain::edge_lens_warp_tuning::kMinSamples +
            static_cast<int>(std::lround(strength * static_cast<float>(
                dof_blur_chain::edge_lens_warp_tuning::kMaxSamples -
                dof_blur_chain::edge_lens_warp_tuning::kMinSamples))),
        dof_blur_chain::edge_lens_warp_tuning::kMinSamples,
        dof_blur_chain::edge_lens_warp_tuning::kMaxSamples);

    const float edge_band_ratio = lerp_float(dof_blur_chain::edge_lens_warp_tuning::kMinEdgeBandRatio,
                                             dof_blur_chain::edge_lens_warp_tuning::kMaxEdgeBandRatio,
                                             strength);
    const float band_x = std::clamp(w * edge_band_ratio, 1.0f, w * 0.48f);
    const float band_y = std::clamp(h * edge_band_ratio, 1.0f, h * 0.48f);
    const float max_push = max_dim * dof_blur_chain::edge_lens_warp_tuning::kMaxEdgePushRatio * strength;
    const float max_scale = dof_blur_chain::edge_lens_warp_tuning::kMaxScaleDelta * strength;

    const SDL_FRect full_src{0.0f, 0.0f, w, h};
    const SDL_FRect full_dst{0.0f, 0.0f, w, h};
    bool ok = draw_weighted_texture_region(renderer,
                                           src,
                                           full_src,
                                           full_dst,
                                           dof_blur_chain::edge_lens_warp_tuning::kBaseWeight);

    auto draw_strip = [&](const SDL_FRect& source, const SDL_FRect& dest, float raw_weight) {
        if (!ok) {
            return;
        }
        ok = draw_weighted_texture_region(renderer, src, source, dest, raw_weight);
    };

    for (int i = 0; i < samples && ok; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(samples);
        const float curved = smoothstep01(t);
        const float push = max_push * curved;
        const float scale = max_scale * curved;
        const float side_weight =
            ((1.0f - t * 0.72f) * (1.0f - t * 0.72f)) *
            dof_blur_chain::edge_lens_warp_tuning::kSideWeight * 0.18f;
        const float corner_weight =
            ((1.0f - t * 0.72f) * (1.0f - t * 0.72f)) *
            dof_blur_chain::edge_lens_warp_tuning::kCornerWeight * 0.14f;

        const float side_push_x = push;
        const float side_push_y = push * 0.72f;
        const float corner_push_x = push * 0.92f;
        const float corner_push_y = push * 0.92f;
        const float grow_x = band_x * scale;
        const float grow_y = band_y * scale;

        const SDL_FRect left_src{0.0f, 0.0f, band_x, h};
        const SDL_FRect right_src{w - band_x, 0.0f, band_x, h};
        const SDL_FRect top_src{0.0f, 0.0f, w, band_y};
        const SDL_FRect bottom_src{0.0f, h - band_y, w, band_y};

        draw_strip(left_src,
                   SDL_FRect{-side_push_x - grow_x, 0.0f, band_x + side_push_x + grow_x, h},
                   side_weight);
        draw_strip(right_src,
                   SDL_FRect{w - band_x, 0.0f, band_x + side_push_x + grow_x, h},
                   side_weight);
        draw_strip(top_src,
                   SDL_FRect{0.0f, -side_push_y - grow_y, w, band_y + side_push_y + grow_y},
                   side_weight);
        draw_strip(bottom_src,
                   SDL_FRect{0.0f, h - band_y, w, band_y + side_push_y + grow_y},
                   side_weight);

        const SDL_FRect tl_src{0.0f, 0.0f, band_x, band_y};
        const SDL_FRect tr_src{w - band_x, 0.0f, band_x, band_y};
        const SDL_FRect bl_src{0.0f, h - band_y, band_x, band_y};
        const SDL_FRect br_src{w - band_x, h - band_y, band_x, band_y};

        draw_strip(tl_src,
                   SDL_FRect{-corner_push_x - grow_x, -corner_push_y - grow_y,
                             band_x + corner_push_x + grow_x, band_y + corner_push_y + grow_y},
                   corner_weight);
        draw_strip(tr_src,
                   SDL_FRect{w - band_x, -corner_push_y - grow_y,
                             band_x + corner_push_x + grow_x, band_y + corner_push_y + grow_y},
                   corner_weight);
        draw_strip(bl_src,
                   SDL_FRect{-corner_push_x - grow_x, h - band_y,
                             band_x + corner_push_x + grow_x, band_y + corner_push_y + grow_y},
                   corner_weight);
        draw_strip(br_src,
                   SDL_FRect{w - band_x, h - band_y,
                             band_x + corner_push_x + grow_x, band_y + corner_push_y + grow_y},
                   corner_weight);
    }

    restore_texture_state(src, src_state);
    render_diagnostics::set_render_target(renderer, previous_target);
    return ok;
}

bool apply_radial_zoom_lens_blur(SDL_Renderer* renderer,
                                 SDL_Texture* src,
                                 SDL_Texture* dst,
                                 int draw_w,
                                 int draw_h,
                                 float radius_px,
                                 SDL_FPoint optical_center) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0 || src == dst) {
        return false;
    }

    if (radius_px <= kEffectEpsilon) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);

    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);

    set_accumulation_blend_mode(src);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const SDL_FPoint center{
        std::clamp(optical_center.x, 0.0f, static_cast<float>(draw_w)),
        std::clamp(optical_center.y, 0.0f, static_cast<float>(draw_h))
    };
    const float max_dim = static_cast<float>(std::max(draw_w, draw_h));
    const float normalized_radius = std::clamp(radius_px / std::max(1.0f, max_dim), 0.0f, 1.0f);
    const int zoom_count = zoom_blur_sample_count(radius_px);
    const float max_zoom_scale_delta =
        std::min(dof_blur_chain::radial_blur_tuning::kMaxScaleDelta,
                 normalized_radius * kZoomBlurScaleMultiplier);

    constexpr float kCenterSampleWeight = 7.0f;
    float total_raw_weight = kCenterSampleWeight;
    std::array<float, dof_blur_chain::radial_blur_tuning::kMaxSamples> zoom_weights{};
    for (int i = 0; i < zoom_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(std::max(1, zoom_count));
        const float w = (1.0f - t) * (1.0f - t);
        zoom_weights[static_cast<std::size_t>(i)] = w;
        total_raw_weight += w;
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
                                kCenterSampleWeight / total_raw_weight);

    for (int i = 0; i < zoom_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(std::max(1, zoom_count));
        const float curved_t = smoothstep01(t);
        const float scale_delta = max_zoom_scale_delta * curved_t;
        const float weight = zoom_weights[static_cast<std::size_t>(i)] / total_raw_weight;

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

bool apply_edge_warp_then_radial_zoom_blur(SDL_Renderer* renderer,
                                           SDL_Texture* src,
                                           SDL_Texture* dst,
                                           SDL_Texture* scratch,
                                           int target_w,
                                           int target_h,
                                           float blur_radius_px,
                                           SDL_FPoint optical_center,
                                           float quality_scale) {
    if (!renderer || !src || !dst || !scratch || target_w <= 0 || target_h <= 0 ||
        scratch == src || scratch == dst) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);
    const TextureStateSnapshot scratch_state = capture_texture_state(scratch);
    const TextureStateSnapshot dst_state = capture_texture_state(dst);

    const float clamped_quality =
        std::clamp(quality_scale, dof_blur_chain::radial_blur_tuning::kMinProcessQualityScale, 1.0f);

    const int process_w =
        std::clamp(static_cast<int>(std::lround(static_cast<float>(target_w) * clamped_quality)), 1, target_w);
    const int process_h =
        std::clamp(static_cast<int>(std::lround(static_cast<float>(target_h) * clamped_quality)), 1, target_h);

    const bool reduced_resolution = (process_w != target_w) || (process_h != target_h);
    const float process_radius = std::max(0.0f, blur_radius_px) * clamped_quality;
    const SDL_FPoint process_optical_center{
        std::clamp(optical_center.x * (static_cast<float>(process_w) / static_cast<float>(target_w)),
                   0.0f,
                   static_cast<float>(process_w)),
        std::clamp(optical_center.y * (static_cast<float>(process_h) / static_cast<float>(target_h)),
                   0.0f,
                   static_cast<float>(process_h))
    };

    if (process_radius <= kEffectEpsilon) {
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

    const float max_dim = static_cast<float>(std::max(process_w, process_h));
    const float warp_strength = std::clamp(process_radius / std::max(1.0f, max_dim * 0.10f), 0.0f, 1.0f);

    SDL_Texture* edge_warped = (current == scratch) ? dst : scratch;
    if (!apply_edge_lens_warp(renderer, current, edge_warped, process_w, process_h, warp_strength)) {
        restore_texture_state(src, src_state);
        restore_texture_state(scratch, scratch_state);
        restore_texture_state(dst, dst_state);
        render_diagnostics::set_render_target(renderer, previous_target);
        return false;
    }

    SDL_Texture* radial_output = (edge_warped == dst) ? scratch : dst;
    if (!apply_radial_zoom_lens_blur(renderer,
                                     edge_warped,
                                     radial_output,
                                     process_w,
                                     process_h,
                                     process_radius,
                                     process_optical_center)) {
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

        if (radial_output == dst) {
            copied_to_dst = copy_texture_region(renderer, dst, scratch, &lowres_rect, &lowres_rect);
            radial_output = scratch;
        }

        if (copied_to_dst) {
            copied_to_dst = copy_texture_region(renderer, radial_output, dst, &lowres_rect, nullptr);
        }
    } else if (radial_output != dst) {
        copied_to_dst = copy_texture_region(renderer, radial_output, dst, nullptr, nullptr);
    }

    restore_texture_state(src, src_state);
    restore_texture_state(scratch, scratch_state);
    restore_texture_state(dst, dst_state);
    render_diagnostics::set_render_target(renderer, previous_target);

    return copied_to_dst;
}

float compute_radial_quality_scale(int width,
                                   int height,
                                   float blur_radius_px,
                                   float normalized_layer_distance,
                                   bool foreground_layer) {
    if (foreground_layer) {
        return 1.0f;
    }

    const float safe_radius = sanitized_non_negative(blur_radius_px);
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

bool enabled(bool depth_of_field_enabled, float blur_px, float radial_blur_px) {
    const bool blur_enabled =
        depth_of_field_enabled &&
        (sanitized_non_negative(blur_px) > kEffectEpsilon ||
         sanitized_non_negative(radial_blur_px) > kEffectEpsilon);

    return blur_enabled || atmospheric_dust_tuning::kEnabled;
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
                         float blur_px,
                         SDL_FPoint optical_center,
                         float radial_blur_px,
                         float quality_scale) const {
    if (!src || !dst || !blur_work) {
        return false;
    }

    const float blur_radius = std::max(sanitized_non_negative(blur_px), sanitized_non_negative(radial_blur_px));
    if (blur_radius <= kEffectEpsilon) {
        return copy_texture(src, dst);
    }

    return apply_edge_warp_then_radial_zoom_blur(renderer_,
                                       src,
                                       dst,
                                       blur_work,
                                       width_,
                                       height_,
                                       blur_radius,
                                       optical_center,
                                       quality_scale);
}

CompositeResult Renderer::compose(const std::vector<LayerTexture>& layers,
                                  SDL_Texture* background_seed,
                                  bool depth_of_field_enabled,
                                  float blur_px,
                                  float radial_blur_px,
                                  SDL_FPoint optical_center,
                                  int focus_depth_layer,
                                  float camera_zoom_percent,
                                  float time_seconds,
                                  DustAnchor dust_anchor) {
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

    const float safe_blur_px = sanitized_non_negative(blur_px);
    const float safe_radial_blur_px = sanitized_non_negative(radial_blur_px);
    const bool blur_enabled =
        depth_of_field_enabled &&
        (safe_blur_px > kEffectEpsilon || safe_radial_blur_px > kEffectEpsilon);

    scratch_background_layers_.clear();
    scratch_foreground_layers_.clear();

    int max_layer_distance = 0;
    float max_layer_world_distance = 0.0f;

    for (const LayerTexture& layer : layers) {
        if (!layer.texture) {
            continue;
        }

        const int layer_distance = std::abs(layer.depth_layer - focus_depth_layer);
        max_layer_distance = std::max(max_layer_distance, layer_distance);
        max_layer_world_distance = std::max(max_layer_world_distance,
                                            layer_world_distance(layer, focus_depth_layer, dust_anchor));

        if (layer.depth_layer > focus_depth_layer) {
            scratch_background_layers_.push_back(layer);
        } else if (layer.depth_layer < focus_depth_layer) {
            scratch_foreground_layers_.push_back(layer);
        }
    }

    if (dust_anchor.max_dust_world_distance > 0.0f) {
        max_layer_world_distance = std::max(max_layer_world_distance, dust_anchor.max_dust_world_distance);
    }

    std::sort(scratch_background_layers_.begin(),
              scratch_background_layers_.end(),
              [](const LayerTexture& a, const LayerTexture& b) {
                  return a.depth_layer > b.depth_layer;
              });

    std::sort(scratch_foreground_layers_.begin(),
              scratch_foreground_layers_.end(),
              [](const LayerTexture& a, const LayerTexture& b) {
                  // Foreground layers must composite from farther-to-nearer (relative to camera)
                  // so the closest layer ends up on top.
                  return a.depth_layer > b.depth_layer;
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
                                 const LayerTexture& layer,
                                 bool foreground_layer,
                                 bool background_seed) -> bool {
        if (!atmospheric_dust_tuning::kEnabled || dust_frames_.empty()) {
            out_source = source;
            return true;
        }

        if (!add_layer_atmospheric_dust(renderer_,
                                        source,
                                        dust_work_,
                                        dust_frames_,
                                        width_,
                                        height_,
                                        layer,
                                        focus_depth_layer,
                                        std::max(1.0f, max_layer_world_distance),
                                        time_seconds,
                                        camera_zoom_percent,
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
        LayerTexture seed_layer{};
        seed_layer.depth_layer = focus_depth_layer + seed_distance;
        seed_layer.blur_strength = 1.0f;
        seed_layer.texture = background_seed;
        seed_layer.world_distance_from_focus = std::max(
            max_layer_world_distance,
            static_cast<float>(seed_distance) * std::max(0.001f, dust_anchor.world_units_per_depth_layer));

        SDL_Texture* seed_source = nullptr;

        if (!add_dust_to_layer(background_seed, seed_source, seed_layer, false, true)) {
            return restore_and_return(CompositeResult{});
        }

        const float seed_blur = blur_enabled ? safe_blur_px * 0.70f : 0.0f;
        const float seed_radial = blur_enabled ? safe_radial_blur_px * 0.70f : 0.0f;

        if (seed_blur > kEffectEpsilon || seed_radial > kEffectEpsilon) {
            const float background_quality = std::clamp(
                compute_radial_quality_scale(width_,
                                             height_,
                                             std::max(seed_blur, seed_radial),
                                             1.0f,
                                             false) *
                    radial_blur_tuning::kBackgroundSeedQualityMultiplier,
                radial_blur_tuning::kMinProcessQualityScale,
                1.0f);

            if (!blur_step(seed_source,
                           background_mid_,
                           blur_work_,
                           seed_blur,
                           optical_center,
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

            if (!add_dust_to_layer(resolved_source, layer_source, layer, foreground_layer, false)) {
                return false;
            }

            const int layer_distance = std::abs(layer.depth_layer - focus_depth_layer);
            const float layer_distance_t =
                std::clamp(static_cast<float>(layer_distance) * inv_max_layer_distance, 0.0f, 1.0f);

            const float layer_strength = std::clamp(layer.blur_strength, 0.0f, 1.0f);
            const float depth_t = depth_curve(layer_distance, layer_distance_t);

            const float layer_blur = blur_enabled ? safe_blur_px * layer_strength * depth_t : 0.0f;
            const float layer_radial = blur_enabled ? safe_radial_blur_px * layer_strength * depth_t : 0.0f;

            SDL_Texture* layer_output = layer_source;

            if (layer_blur > kEffectEpsilon || layer_radial > kEffectEpsilon) {
                const float layer_quality =
                    compute_radial_quality_scale(width_,
                                                 height_,
                                                 std::max(layer_blur, layer_radial),
                                                 layer_distance_t,
                                                 foreground_layer);

                if (!blur_step(layer_source,
                               foreground_layer_,
                               blur_work_,
                               layer_blur,
                               optical_center,
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

        if (!add_dust_to_layer(resolved_source, layer_source, layer, false, false)) {
            return restore_and_return(CompositeResult{});
        }

        constexpr float kFocusLayerRadialMultiplier = 0.025f;

        const float focus_blur =
            blur_enabled
                ? safe_blur_px *
                      std::clamp(layer.blur_strength, 0.0f, 1.0f) *
                      kFocusLayerRadialMultiplier
                : 0.0f;

        const float focus_radial =
            blur_enabled
                ? safe_radial_blur_px *
                      std::clamp(layer.blur_strength, 0.0f, 1.0f) *
                      kFocusLayerRadialMultiplier
                : 0.0f;

        SDL_Texture* output = layer_source;

        if (focus_blur > kEffectEpsilon || focus_radial > kEffectEpsilon) {
            if (!blur_step(layer_source,
                           foreground_layer_,
                           blur_work_,
                           focus_blur,
                           optical_center,
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
