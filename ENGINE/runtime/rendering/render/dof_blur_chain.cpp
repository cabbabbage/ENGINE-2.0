#include "rendering/render/dof_blur_chain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/screen_space_math.hpp"

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

struct WorldPoint3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool valid = false;
};

struct CameraRay {
    WorldPoint3 origin{};
    WorldPoint3 direction{};
    bool valid = false;
};

struct WorldBounds {
    float min_x = 0.0f;
    float max_x = 0.0f;
    float min_y = 0.0f;
    float max_y = 0.0f;
    bool valid = false;
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

bool build_camera_ray_from_screen(const world::CameraProjectionParams& params,
                                  SDL_FPoint screen_point,
                                  CameraRay& out_ray) {
    out_ray = CameraRay{};
    const double safe_scale = std::max(1.0e-6, params.meters_scale);
    if (!std::isfinite(safe_scale)) {
        return false;
    }

    const auto [ndc_x, ndc_y] = render::screen_space::screen_to_ndc(
        static_cast<double>(screen_point.x),
        static_cast<double>(screen_point.y),
        params.screen_width,
        params.screen_height,
        params.screen_zoom,
        params.screen_pan_y_px);
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
        return false;
    }

    const double dir_x = params.forward_x + params.right_x * (ndc_x * params.tan_half_fov_x) +
                         params.up_x * (ndc_y * params.tan_half_fov_y);
    const double dir_y = params.forward_y + params.right_y * (ndc_x * params.tan_half_fov_x) +
                         params.up_y * (ndc_y * params.tan_half_fov_y);
    const double dir_z = params.forward_z + params.right_z * (ndc_x * params.tan_half_fov_x) +
                         params.up_z * (ndc_y * params.tan_half_fov_y);
    const double len = std::sqrt(dir_x * dir_x + dir_y * dir_y + dir_z * dir_z);
    if (!std::isfinite(len) || len <= 1.0e-8) {
        return false;
    }

    const double inv_len = 1.0 / len;
    out_ray.origin = WorldPoint3{
        static_cast<float>(params.position_x / safe_scale + params.anchor_world_x),
        static_cast<float>(params.position_y / safe_scale + params.anchor_world_y),
        static_cast<float>(params.position_z / safe_scale + params.anchor_world_z),
        true};
    out_ray.direction = WorldPoint3{
        static_cast<float>(dir_x * inv_len),
        static_cast<float>(dir_y * inv_len),
        static_cast<float>(dir_z * inv_len),
        true};
    out_ray.valid =
        std::isfinite(out_ray.origin.x) &&
        std::isfinite(out_ray.origin.y) &&
        std::isfinite(out_ray.origin.z) &&
        std::isfinite(out_ray.direction.x) &&
        std::isfinite(out_ray.direction.y) &&
        std::isfinite(out_ray.direction.z);
    return out_ray.valid;
}

bool intersect_camera_ray_on_world_z(const world::CameraProjectionParams& params,
                                     const CameraRay& ray,
                                     float target_world_z,
                                     WorldPoint3& out_world) {
    out_world = WorldPoint3{};
    if (!ray.valid) {
        return false;
    }

    const double safe_scale = std::max(1.0e-6, params.meters_scale);
    if (!std::isfinite(safe_scale) || std::fabs(static_cast<double>(ray.direction.z)) <= 1.0e-8) {
        return false;
    }

    const double target_z_m = (static_cast<double>(target_world_z) - params.anchor_world_z) * safe_scale;
    const double t = (target_z_m - params.position_z) / static_cast<double>(ray.direction.z);
    if (!std::isfinite(t) || t <= 0.0) {
        return false;
    }

    out_world = WorldPoint3{
        static_cast<float>((params.position_x + static_cast<double>(ray.direction.x) * t) / safe_scale +
                           params.anchor_world_x),
        static_cast<float>((params.position_y + static_cast<double>(ray.direction.y) * t) / safe_scale +
                           params.anchor_world_y),
        target_world_z,
        true};
    out_world.valid =
        std::isfinite(out_world.x) &&
        std::isfinite(out_world.y) &&
        std::isfinite(out_world.z);
    return out_world.valid;
}

bool project_world_to_screen(const world::CameraProjectionParams& params,
                             const WorldPoint3& world,
                             SDL_FPoint& out_screen) {
    if (!world.valid) {
        return false;
    }

    const double safe_scale = std::max(1.0e-6, params.meters_scale);
    if (!std::isfinite(safe_scale)) {
        return false;
    }

    const double wx = (static_cast<double>(world.x) - params.anchor_world_x) * safe_scale;
    const double wy = (static_cast<double>(world.y) - params.anchor_world_y) * safe_scale;
    const double wz = (static_cast<double>(world.z) - params.anchor_world_z) * safe_scale;
    const double to_x = wx - params.position_x;
    const double to_y = wy - params.position_y;
    const double to_z = wz - params.position_z;
    const double depth = to_x * params.forward_x + to_y * params.forward_y + to_z * params.forward_z;
    if (!std::isfinite(depth) || depth <= params.near_plane || depth > params.far_plane) {
        return false;
    }

    const double cam_x = to_x * params.right_x + to_y * params.right_y + to_z * params.right_z;
    const double cam_y = to_x * params.up_x + to_y * params.up_y + to_z * params.up_z;
    const double ndc_x = cam_x / (depth * std::max(1.0e-6, params.tan_half_fov_x));
    const double ndc_y = cam_y / (depth * std::max(1.0e-6, params.tan_half_fov_y));
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
        return false;
    }

    out_screen = render::screen_space::ndc_to_screen(ndc_x,
                                                     ndc_y,
                                                     params.screen_width,
                                                     params.screen_height,
                                                     params.screen_zoom,
                                                     params.screen_pan_y_px);
    return std::isfinite(out_screen.x) && std::isfinite(out_screen.y);
}

bool expand_bounds_from_screen_sample(const world::CameraProjectionParams& params,
                                      SDL_FPoint screen,
                                      float world_z,
                                      WorldBounds& bounds) {
    CameraRay ray{};
    WorldPoint3 world{};
    if (!build_camera_ray_from_screen(params, screen, ray) ||
        !intersect_camera_ray_on_world_z(params, ray, world_z, world)) {
        return false;
    }

    if (!bounds.valid) {
        bounds.min_x = bounds.max_x = world.x;
        bounds.min_y = bounds.max_y = world.y;
        bounds.valid = true;
    } else {
        bounds.min_x = std::min(bounds.min_x, world.x);
        bounds.max_x = std::max(bounds.max_x, world.x);
        bounds.min_y = std::min(bounds.min_y, world.y);
        bounds.max_y = std::max(bounds.max_y, world.y);
    }
    return true;
}

WorldBounds visible_world_bounds_for_plane(const world::CameraProjectionParams& params,
                                           int width,
                                           int height,
                                           float world_z,
                                           float pad_x,
                                           float pad_y) {
    WorldBounds bounds{};
    const float w = static_cast<float>(std::max(1, width));
    const float h = static_cast<float>(std::max(1, height));
    const std::array<SDL_FPoint, 9> samples{
        SDL_FPoint{0.0f, 0.0f},
        SDL_FPoint{w * 0.5f, 0.0f},
        SDL_FPoint{w, 0.0f},
        SDL_FPoint{0.0f, h * 0.5f},
        SDL_FPoint{w * 0.5f, h * 0.5f},
        SDL_FPoint{w, h * 0.5f},
        SDL_FPoint{0.0f, h},
        SDL_FPoint{w * 0.5f, h},
        SDL_FPoint{w, h}
    };

    for (const SDL_FPoint& sample : samples) {
        (void)expand_bounds_from_screen_sample(params, sample, world_z, bounds);
    }

    if (bounds.valid) {
        bounds.min_x -= pad_x;
        bounds.max_x += pad_x;
        bounds.min_y -= pad_y;
        bounds.max_y += pad_y;
    }
    return bounds;
}

float dust_alpha_for_world_distance(float world_distance, float max_world_distance) {
    if (max_world_distance <= 0.0f) {
        return 1.0f;
    }
    if (world_distance >= max_world_distance) {
        return 0.0f;
    }

    const float fade_start = max_world_distance * 0.80f;
    if (world_distance <= fade_start) {
        return 1.0f;
    }

    const float fade_width = std::max(1.0e-4f, max_world_distance - fade_start);
    return smoothstep01((max_world_distance - world_distance) / fade_width);
}

bool resolve_reference_tile_world_size(const dof_blur_chain::DustAnchor& dust_anchor,
                                       float source_w,
                                       float source_h,
                                       float& out_world_w,
                                       float& out_world_h) {
    out_world_w = 0.0f;
    out_world_h = 0.0f;
    if (!dust_anchor.has_projection || source_w <= 0.0f || source_h <= 0.0f) {
        return false;
    }

    const world::CameraProjectionParams& projection = dust_anchor.projection;
    const SDL_FPoint center{
        static_cast<float>(std::max(1, projection.screen_width)) * 0.5f,
        static_cast<float>(std::max(1, projection.screen_height)) * 0.5f
    };
    const float ref_screen_w = source_w * dof_blur_chain::atmospheric_dust_tuning::kFocusTileScale;
    const float ref_screen_h = source_h * dof_blur_chain::atmospheric_dust_tuning::kFocusTileScale;

    CameraRay center_ray{};
    CameraRay right_ray{};
    CameraRay up_ray{};
    WorldPoint3 center_world{};
    WorldPoint3 right_world{};
    WorldPoint3 up_world{};
    if (!build_camera_ray_from_screen(projection, center, center_ray) ||
        !build_camera_ray_from_screen(projection, SDL_FPoint{center.x + ref_screen_w, center.y}, right_ray) ||
        !build_camera_ray_from_screen(projection, SDL_FPoint{center.x, center.y - ref_screen_h}, up_ray) ||
        !intersect_camera_ray_on_world_z(projection, center_ray, dust_anchor.focus_world_z, center_world) ||
        !intersect_camera_ray_on_world_z(projection, right_ray, dust_anchor.focus_world_z, right_world) ||
        !intersect_camera_ray_on_world_z(projection, up_ray, dust_anchor.focus_world_z, up_world)) {
        return false;
    }

    out_world_w = std::abs(right_world.x - center_world.x);
    out_world_h = std::abs(up_world.y - center_world.y);
    return std::isfinite(out_world_w) && std::isfinite(out_world_h) &&
           out_world_w > 0.0f && out_world_h > 0.0f;
}

bool draw_world_depth_dust_tiles(SDL_Renderer* renderer,
                                 const std::vector<SDL_Texture*>& dust_frames,
                                 const dof_blur_chain::DustAnchor& dust_anchor,
                                 int width,
                                 int height,
                                 int depth_layer,
                                 float world_z,
                                 float tile_world_w,
                                 float tile_world_h,
                                 float alpha,
                                 float time_seconds) {
    if (!renderer || dust_frames.empty() || !dust_anchor.has_projection ||
        width <= 0 || height <= 0 || tile_world_w <= 0.0f || tile_world_h <= 0.0f ||
        alpha <= 0.0f) {
        return true;
    }

    const int frame_count = static_cast<int>(dust_frames.size());
    if (frame_count <= 0) {
        return true;
    }

    WorldBounds bounds = visible_world_bounds_for_plane(dust_anchor.projection,
                                                       width,
                                                       height,
                                                       world_z,
                                                       tile_world_w * 2.0f,
                                                       tile_world_h * 2.0f);
    if (!bounds.valid) {
        return true;
    }

    int tile_x0 = static_cast<int>(std::floor(bounds.min_x / tile_world_w));
    int tile_x1 = static_cast<int>(std::ceil(bounds.max_x / tile_world_w));
    int tile_y0 = static_cast<int>(std::floor(bounds.min_y / tile_world_h));
    int tile_y1 = static_cast<int>(std::ceil(bounds.max_y / tile_world_h));

    constexpr int kMaxTilesPerAxis = 160;
    if (tile_x1 - tile_x0 > kMaxTilesPerAxis) {
        const int center = (tile_x0 + tile_x1) / 2;
        tile_x0 = center - kMaxTilesPerAxis / 2;
        tile_x1 = tile_x0 + kMaxTilesPerAxis;
    }
    if (tile_y1 - tile_y0 > kMaxTilesPerAxis) {
        const int center = (tile_y0 + tile_y1) / 2;
        tile_y0 = center - kMaxTilesPerAxis / 2;
        tile_y1 = tile_y0 + kMaxTilesPerAxis;
    }

    const int base_frame =
        static_cast<int>(std::floor(std::max(0.0f, time_seconds) *
                                    dof_blur_chain::atmospheric_dust_tuning::kAnimationFps));
    const Uint8 alpha_mod = static_cast<Uint8>(std::clamp(
        static_cast<int>(std::lround(alpha *
                                     dof_blur_chain::atmospheric_dust_tuning::kDrawAlpha *
                                     255.0f)),
        0,
        255));
    if (alpha_mod == 0) {
        return true;
    }

    bool ok = true;
    const int indices[6]{0, 1, 2, 0, 2, 3};

    for (int ty = tile_y0; ty <= tile_y1 && ok; ++ty) {
        for (int tx = tile_x0; tx <= tile_x1 && ok; ++tx) {
            const std::uint32_t h = hash_tile(depth_layer, tx, ty, 211);
            const int frame_offset = static_cast<int>(h % static_cast<std::uint32_t>(frame_count));
            SDL_Texture* dust = dust_frames[static_cast<std::size_t>(
                ping_pong_frame_index(base_frame + frame_offset, frame_count))];
            if (!dust) {
                continue;
            }

            const float x0 = static_cast<float>(tx) * tile_world_w;
            const float x1 = static_cast<float>(tx + 1) * tile_world_w;
            const float y0 = static_cast<float>(ty) * tile_world_h;
            const float y1 = static_cast<float>(ty + 1) * tile_world_h;

            SDL_FPoint tl{}, tr{}, br{}, bl{};
            if (!project_world_to_screen(dust_anchor.projection, WorldPoint3{x0, y1, world_z, true}, tl) ||
                !project_world_to_screen(dust_anchor.projection, WorldPoint3{x1, y1, world_z, true}, tr) ||
                !project_world_to_screen(dust_anchor.projection, WorldPoint3{x1, y0, world_z, true}, br) ||
                !project_world_to_screen(dust_anchor.projection, WorldPoint3{x0, y0, world_z, true}, bl)) {
                continue;
            }

            const SDL_FlipMode flip = tile_flip_mode(depth_layer, tx, ty);
            const int flip_flags = static_cast<int>(flip);
            float u0 = 0.0f;
            float u1 = 1.0f;
            float v0 = 0.0f;
            float v1 = 1.0f;
            if ((flip_flags & static_cast<int>(SDL_FLIP_HORIZONTAL)) != 0) {
                std::swap(u0, u1);
            }
            if ((flip_flags & static_cast<int>(SDL_FLIP_VERTICAL)) != 0) {
                std::swap(v0, v1);
            }

            SDL_Vertex vertices[4]{};
            vertices[0].position = tl;
            vertices[0].tex_coord = SDL_FPoint{u0, v0};
            vertices[0].color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
            vertices[1].position = tr;
            vertices[1].tex_coord = SDL_FPoint{u1, v0};
            vertices[1].color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
            vertices[2].position = br;
            vertices[2].tex_coord = SDL_FPoint{u1, v1};
            vertices[2].color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
            vertices[3].position = bl;
            vertices[3].tex_coord = SDL_FPoint{u0, v1};
            vertices[3].color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};

            const TextureStateSnapshot state = capture_texture_state(dust);
            SDL_SetTextureBlendMode(dust, SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(dust, alpha_mod);
            SDL_SetTextureColorMod(dust, 255, 255, 255);
            ok = render_diagnostics::render_geometry(renderer, dust, vertices, 4, indices, 6);
            restore_texture_state(dust, state);
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
                                bool background_seed,
                                dof_blur_chain::DustAnchor dust_anchor) {
    (void)max_layer_world_distance;
    (void)background_seed;

    if (!renderer || !src || !dst || width <= 0 || height <= 0 || src == dst) {
        return false;
    }

    if (!copy_texture_region(renderer, src, dst, nullptr, nullptr)) {
        return false;
    }

    if (!dof_blur_chain::atmospheric_dust_tuning::kEnabled ||
        dust_frames.empty() ||
        !dust_anchor.has_projection) {
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

    const float world_distance = layer_world_distance(layer, focus_depth_layer, dust_anchor);

    const float alpha = dust_alpha_for_world_distance(world_distance, dust_anchor.max_dust_world_distance);
    if (alpha <= 0.0f) {
        return true;
    }

    float tile_world_w = 0.0f;
    float tile_world_h = 0.0f;
    if (!resolve_reference_tile_world_size(dust_anchor, source_w, source_h, tile_world_w, tile_world_h)) {
        const float fallback = std::max(1.0f, dust_anchor.world_units_per_depth_layer);
        tile_world_w = std::max(1.0f,
                                source_w *
                                    dof_blur_chain::atmospheric_dust_tuning::kFocusTileScale *
                                    fallback / 128.0f);
        tile_world_h = std::max(1.0f,
                                source_h *
                                    dof_blur_chain::atmospheric_dust_tuning::kFocusTileScale *
                                    fallback / 128.0f);
    }

    const float world_z = layer.has_dust_world_z
        ? layer.dust_world_z
        : render_depth::world_z_from_depth_offset(world_distance,
                                                  dust_anchor.focus_world_z,
                                                  dust_anchor.depth_axis_sign);

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    render_diagnostics::set_render_target(renderer, dst);

    const bool ok = draw_world_depth_dust_tiles(renderer,
                                                dust_frames,
                                                dust_anchor,
                                                width,
                                                height,
                                                layer.depth_layer,
                                                world_z,
                                                tile_world_w,
                                                tile_world_h,
                                                alpha,
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
    (void)camera_zoom_percent;

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
                                 bool /*foreground_layer*/,
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
        if (dust_anchor.max_dust_world_distance > 0.0f) {
            seed_layer.world_distance_from_focus =
                std::min(seed_layer.world_distance_from_focus, dust_anchor.max_dust_world_distance * 0.95f);
        }
        seed_layer.dust_world_z = render_depth::world_z_from_depth_offset(seed_layer.world_distance_from_focus,
                                                                          dust_anchor.focus_world_z,
                                                                          dust_anchor.depth_axis_sign);
        seed_layer.has_dust_world_z = true;

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
