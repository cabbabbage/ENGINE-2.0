#include "AnchorPointResolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <string>
#include <cctype>
#include <utility>

#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/world_grid.hpp"
#include "rendering/render/projected_sprite_frame.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/log.hpp"

namespace {

#if defined(ENGINE_WORLD_TESTS)

}  // namespace

namespace anchor_points {

float anchor_height_px(const Asset& asset) {
    return asset.runtime_height_px();
}

bool compute_camera_to_point_ray(const Asset& asset,
                                 const AnchorWorldPoint3& flat_point,
                                 AnchorWorldPoint3& out_direction) {
    out_direction = AnchorWorldPoint3{};
    if (!flat_point.valid) {
        return false;
    }

    const float origin_x = static_cast<float>(asset.world_x());
    const float origin_y = static_cast<float>(asset.world_y());
    const float origin_z = static_cast<float>(asset.world_z());
    const float ray_x = flat_point.x - origin_x;
    const float ray_y = flat_point.y - origin_y;
    const float ray_z = flat_point.z - origin_z;
    const float ray_len_sq = ray_x * ray_x + ray_y * ray_y + ray_z * ray_z;
    if (ray_len_sq <= 1e-10f || !std::isfinite(ray_len_sq)) {
        out_direction = AnchorWorldPoint3{0.0f, 0.0f, 1.0f, true};
        return true;
    }

    const float inv_len = 1.0f / std::sqrt(ray_len_sq);
    out_direction = AnchorWorldPoint3{
        ray_x * inv_len,
        ray_y * inv_len,
        ray_z * inv_len,
        true};
    return std::isfinite(out_direction.x) &&
           std::isfinite(out_direction.y) &&
           std::isfinite(out_direction.z);
}

bool displace_along_camera_to_point_ray(const Asset& asset,
                                        const AnchorWorldPoint3& flat_point,
                                        float signed_offset,
                                        AnchorWorldPoint3& out_point,
                                        AnchorWorldPoint3* out_direction) {
    out_point = flat_point;
    out_point.valid = flat_point.valid;
    if (!flat_point.valid || !std::isfinite(signed_offset)) {
        return false;
    }

    AnchorWorldPoint3 direction{};
    if (!compute_camera_to_point_ray(asset, flat_point, direction)) {
        return false;
    }
    if (out_direction) {
        *out_direction = direction;
    }

    out_point.x = flat_point.x + direction.x * signed_offset;
    out_point.y = flat_point.y + direction.y * signed_offset;
    out_point.z = flat_point.z + direction.z * signed_offset;
    out_point.valid = std::isfinite(out_point.x) &&
                      std::isfinite(out_point.y) &&
                      std::isfinite(out_point.z);
    return out_point.valid;
}

bool build_asymmetric_camera_ray_extrusion(const Asset& asset,
                                           const AnchorWorldPoint3& flat_point,
                                           float extrusion_backward,
                                           float extrusion_forward,
                                           AnchorWorldPoint3& out_near_point,
                                           AnchorWorldPoint3& out_far_point,
                                           AnchorWorldPoint3* out_direction) {
    if (!std::isfinite(extrusion_backward) ||
        !std::isfinite(extrusion_forward) ||
        extrusion_backward < 0.0f ||
        extrusion_forward < 0.0f) {
        return false;
    }

    AnchorWorldPoint3 direction{};
    if (!compute_camera_to_point_ray(asset, flat_point, direction)) {
        return false;
    }
    if (out_direction) {
        *out_direction = direction;
    }

    out_near_point = AnchorWorldPoint3{
        flat_point.x - direction.x * extrusion_backward,
        flat_point.y - direction.y * extrusion_backward,
        flat_point.z - direction.z * extrusion_backward,
        true};
    out_far_point = AnchorWorldPoint3{
        flat_point.x + direction.x * extrusion_forward,
        flat_point.y + direction.y * extrusion_forward,
        flat_point.z + direction.z * extrusion_forward,
        true};
    const bool valid = std::isfinite(out_near_point.x) &&
                       std::isfinite(out_near_point.y) &&
                       std::isfinite(out_near_point.z) &&
                       std::isfinite(out_far_point.x) &&
                       std::isfinite(out_far_point.y) &&
                       std::isfinite(out_far_point.z);
    out_near_point.valid = valid;
    out_far_point.valid = valid;
    return valid;
}

FrameAnchorSample resolve_frame_anchor_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization) {
    FrameAnchorSample sample{};
    const float flat_world_z = static_cast<float>(asset.world_z()) + asset.world_z_offset();
    sample.resolved.world_exact_pos_2d = Vec2{
        static_cast<float>(asset.world_x()),
        static_cast<float>(asset.world_y())};
    sample.resolved.world_exact_z = flat_world_z;
    sample.resolved.world_px = asset.world_xy_point();
    sample.resolved.world_z = static_cast<int>(std::lround(flat_world_z));
    sample.resolved.world_depth = flat_world_z;
    sample.resolved.depth_offset = anchor.depth_offset;
    sample.screen_px = SDL_FPoint{static_cast<float>(asset.world_x()), static_cast<float>(asset.world_y())};
    sample.flat_screen_px = sample.screen_px;
    sample.has_flat_screen_px = true;
    sample.final_screen_px = sample.screen_px;
    sample.has_final_screen_px = true;
    sample.flat_relative_pixel_point = AnchorWorldPoint3{
        static_cast<float>(sample.resolved.world_px.x),
        static_cast<float>(sample.resolved.world_px.y),
        flat_world_z,
        true};
    sample.resolved.flat_world_exact_pos_2d = Vec2{
        sample.flat_relative_pixel_point.x,
        sample.flat_relative_pixel_point.y};
    sample.resolved.flat_world_exact_z = sample.flat_relative_pixel_point.z;
    const Asset::PerspectiveSample flat_perspective_sample = asset.runtime_perspective_sample();
    if (std::isfinite(flat_perspective_sample.scale) && flat_perspective_sample.scale > 0.0001f) {
        sample.resolved.flat_perspective_scale = std::max(0.0001f, flat_perspective_sample.scale);
        sample.resolved.has_flat_perspective_scale = true;
    }
    sample.final_anchor_point = sample.flat_relative_pixel_point;
    displace_along_camera_to_point_ray(asset,
                                       sample.flat_relative_pixel_point,
                                       static_cast<float>(anchor.depth_offset),
                                       sample.final_anchor_point);
    if (!anchor.resolve_x && sample.flat_relative_pixel_point.valid && sample.final_anchor_point.valid) {
        // Keep ray-derived depth, but pin horizontal world position to the flat source point.
        // In ENGINE_WORLD_TESTS we treat screen-Y as world-Y, so matching screen height means
        // restoring the flat point's world Y.
        sample.final_anchor_point.x = sample.flat_relative_pixel_point.x;
        sample.final_anchor_point.y = sample.flat_relative_pixel_point.y;
        sample.final_anchor_point.valid =
            std::isfinite(sample.final_anchor_point.x) &&
            std::isfinite(sample.final_anchor_point.y) &&
            std::isfinite(sample.final_anchor_point.z);
    }
    sample.resolved.world_exact_pos_2d = Vec2{
        sample.final_anchor_point.x,
        sample.final_anchor_point.y};
    sample.resolved.world_exact_z = sample.final_anchor_point.z;
    sample.resolved.world_px = SDL_Point{
        static_cast<int>(std::lround(sample.final_anchor_point.x)),
        static_cast<int>(std::lround(sample.final_anchor_point.y))};
    sample.resolved.world_z = static_cast<int>(std::lround(sample.final_anchor_point.z));
    sample.resolved.world_depth = sample.final_anchor_point.z;
    sample.resolved.source_texture_px = SDL_Point{anchor.texture_x, anchor.texture_y};
    sample.resolved.has_canonical_texture_source = true;
    sample.resolved.missing = false;
    return sample;
}

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    const auto sample = resolve_frame_anchor_sample(asset, anchor, grid_policy);
    return PixelLockedAnchor{sample.resolved, sample.screen_px};
}

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy) {
    return resolve_frame_anchor_sample(asset, anchor, grid_policy).resolved;
}

}

#else

struct FrameDimensions {
    int frame_w = 0;
    int frame_h = 0;
    int final_w = 0;
    int final_h = 0;
    SDL_FlipMode flip = SDL_FLIP_NONE;
    double angle = 0.0;
    float world_z_offset = 0.0f;
};

struct AnchorFrameSample {
    SDL_Point scaled_px{0, 0};
    SDL_Point source_px{0, 0};
};

struct WorldPoint3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

std::optional<WorldPoint3> camera_world_position(const Assets* assets_owner) {
    if (!assets_owner) {
        return std::nullopt;
    }

    const auto params = assets_owner->getView().projection_params();
    const double meters_scale = std::max(1e-6, params.meters_scale);
    if (!std::isfinite(meters_scale)) {
        return std::nullopt;
    }

    WorldPoint3 world_camera_position{};
    world_camera_position.x = static_cast<float>(params.position_x / meters_scale + params.anchor_world_x);
    world_camera_position.y = static_cast<float>(params.position_y / meters_scale + params.anchor_world_y);
    world_camera_position.z = static_cast<float>(params.position_z / meters_scale + params.anchor_world_z);

    if (!std::isfinite(world_camera_position.x) ||
        !std::isfinite(world_camera_position.y) ||
        !std::isfinite(world_camera_position.z)) {
        return std::nullopt;
    }

    return world_camera_position;
}

bool compute_camera_to_point_direction(const Assets* assets_owner,
                                       const WorldPoint3& flat_relative_pixel_point,
                                       WorldPoint3& out_direction) {
    const auto camera_position_opt = camera_world_position(assets_owner);
    if (!camera_position_opt.has_value()) {
        return false;
    }

    const WorldPoint3 camera_position = *camera_position_opt;
    const float camera_to_point_x = flat_relative_pixel_point.x - camera_position.x;
    const float camera_to_point_y = flat_relative_pixel_point.y - camera_position.y;
    const float camera_to_point_z = flat_relative_pixel_point.z - camera_position.z;

    const float ray_length_sq =
        camera_to_point_x * camera_to_point_x +
        camera_to_point_y * camera_to_point_y +
        camera_to_point_z * camera_to_point_z;
    if (ray_length_sq <= 1e-10f || !std::isfinite(ray_length_sq)) {
        return false;
    }

    const float inv_ray_length = 1.0f / std::sqrt(ray_length_sq);
    out_direction.x = camera_to_point_x * inv_ray_length;
    out_direction.y = camera_to_point_y * inv_ray_length;
    out_direction.z = camera_to_point_z * inv_ray_length;
    return std::isfinite(out_direction.x) &&
           std::isfinite(out_direction.y) &&
           std::isfinite(out_direction.z);
}

bool apply_depth_offset_along_camera_ray(const Assets* assets_owner,
                                         float depth_offset,
                                         const WorldPoint3& flat_relative_pixel_point,
                                         WorldPoint3& final_anchor_point,
                                         WorldPoint3* out_direction = nullptr) {
    final_anchor_point = flat_relative_pixel_point;
    if (!std::isfinite(depth_offset)) {
        return false;
    }

    WorldPoint3 camera_to_point_dir{};
    if (!compute_camera_to_point_direction(assets_owner, flat_relative_pixel_point, camera_to_point_dir)) {
        return false;
    }
    if (out_direction) {
        *out_direction = camera_to_point_dir;
    }

    final_anchor_point.x = flat_relative_pixel_point.x + camera_to_point_dir.x * depth_offset;
    final_anchor_point.y = flat_relative_pixel_point.y + camera_to_point_dir.y * depth_offset;
    final_anchor_point.z = flat_relative_pixel_point.z + camera_to_point_dir.z * depth_offset;

    return std::isfinite(final_anchor_point.x) &&
           std::isfinite(final_anchor_point.y) &&
           std::isfinite(final_anchor_point.z);
}

bool vibble_scale_trace_enabled() {
    static const bool enabled = [] {
        const char* raw = SDL_getenv("VIBBLE_SCALE_TRACE");
        if (!raw || !*raw) {
            return false;
        }
        const std::string value(raw);
        return value == "1" ||
               value == "true" ||
               value == "TRUE" ||
               value == "on" ||
               value == "ON";
    }();
    return enabled;
}

bool is_vibble_trace_asset(const Asset& asset) {
    if (!asset.info) {
        return false;
    }
    std::string lowered = asset.info->name;
    for (char& ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered.find("vibble") != std::string::npos;
}

bool should_trace_anchor_resolver(const Asset& asset) {
    return vibble_scale_trace_enabled() && is_vibble_trace_asset(asset);
}

bool should_emit_anchor_trace_for_camera_state(const Asset& asset, std::uint64_t camera_state_version) {
    static std::unordered_map<const Asset*, std::uint64_t> last_logged_state;
    auto it = last_logged_state.find(&asset);
    if (it != last_logged_state.end() && it->second == camera_state_version) {
        return false;
    }
    last_logged_state[&asset] = camera_state_version;
    return true;
}

bool should_warn_perspective_divergence(const Asset& asset, std::uint64_t camera_state_version) {
    static std::unordered_map<const Asset*, std::uint64_t> last_warn_state;
    auto it = last_warn_state.find(&asset);
    if (it != last_warn_state.end() && it->second == camera_state_version) {
        return false;
    }
    last_warn_state[&asset] = camera_state_version;
    return true;
}

float safe_remainder_scale(const Asset& asset) {
    float remainder = asset.current_remaining_scale_adjustment;
    if (!std::isfinite(remainder) || remainder <= 0.0f) {
        remainder = 1.0f;
    }
    return remainder;
}

bool gather_frame_dimensions(const Asset& asset, FrameDimensions& out) {
    const AnimationFrame* frame = asset.current_frame;
    SDL_Texture* texture = nullptr;
    int frame_w = 0;
    int frame_h = 0;

    if (frame && !frame->variants.empty()) {
        const int variant_idx = std::clamp(asset.current_variant_index, 0, static_cast<int>(frame->variants.size()) - 1);
        const FrameVariant& variant = frame->variants[static_cast<std::size_t>(variant_idx)];
        texture = variant.get_base_texture();
        if (variant.source_rect.w > 0 && variant.source_rect.h > 0) {
            frame_w = variant.source_rect.w;
            frame_h = variant.source_rect.h;
        }
    }

    if (!texture) {
        texture = asset.get_current_frame();
    }

    if (frame_w <= 0 || frame_h <= 0) {
        float tex_w = 0.0f;
        float tex_h = 0.0f;
        if (texture && SDL_GetTextureSize(texture, &tex_w, &tex_h)) {
            frame_w = static_cast<int>(std::lround(tex_w));
            frame_h = static_cast<int>(std::lround(tex_h));
        }
    }

    if (frame_w <= 0 || frame_h <= 0) {
        return false;
    }

    const float remainder = safe_remainder_scale(asset);
    out.frame_w = frame_w;
    out.frame_h = frame_h;
    out.final_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(frame_w) * remainder)));
    out.final_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(frame_h) * remainder)));
    out.flip = asset.effective_render_flip();
    out.angle = asset.effective_render_angle();
    if (!std::isfinite(out.angle)) {
        out.angle = 0.0;
    }
    out.world_z_offset = asset.world_z_offset();
    return true;
}

AnchorFrameSample compute_anchor_frame_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              const FrameDimensions& dims) {
    (void)dims;
    // Anchors are authored in canonical texture space. Map to the active runtime variant first.
    const float variant_scale = (std::isfinite(asset.current_nearest_variant_scale) &&
                                 asset.current_nearest_variant_scale > 0.0f)
                                    ? asset.current_nearest_variant_scale
                                    : 1.0f;

    const float scaled_x_f = static_cast<float>(anchor.texture_x) * variant_scale;
    const float scaled_y_f = static_cast<float>(anchor.texture_y) * variant_scale;

    SDL_Point scaled_px{
        static_cast<int>(std::lround(scaled_x_f)),
        static_cast<int>(std::lround(scaled_y_f))
    };

    return AnchorFrameSample{scaled_px, SDL_Point{anchor.texture_x, anchor.texture_y}};
}

}  // namespace

namespace anchor_points {

float anchor_height_px(const Asset& asset) {
    const float runtime_height = asset.runtime_height_px();
    if (std::isfinite(runtime_height) && runtime_height > 0.0f) {
        return runtime_height;
    }
    const int h = asset.height();
    return (h > 0) ? static_cast<float>(h) : 0.0f;
}

bool compute_camera_to_point_ray(const Asset& asset,
                                 const AnchorWorldPoint3& flat_point,
                                 AnchorWorldPoint3& out_direction) {
    out_direction = AnchorWorldPoint3{};
    if (!flat_point.valid) {
        return false;
    }

    Assets* assets_owner = asset.get_assets();
    if (!assets_owner) {
        return false;
    }

    WorldPoint3 direction{};
    if (!compute_camera_to_point_direction(
            assets_owner,
            WorldPoint3{flat_point.x, flat_point.y, flat_point.z},
            direction)) {
        return false;
    }

    out_direction = AnchorWorldPoint3{
        direction.x,
        direction.y,
        direction.z,
        std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z)};
    return out_direction.valid;
}

bool displace_along_camera_to_point_ray(const Asset& asset,
                                        const AnchorWorldPoint3& flat_point,
                                        float signed_offset,
                                        AnchorWorldPoint3& out_point,
                                        AnchorWorldPoint3* out_direction) {
    out_point = flat_point;
    out_point.valid = flat_point.valid;
    if (!flat_point.valid || !std::isfinite(signed_offset)) {
        return false;
    }

    AnchorWorldPoint3 direction{};
    if (!compute_camera_to_point_ray(asset, flat_point, direction)) {
        return false;
    }
    if (out_direction) {
        *out_direction = direction;
    }

    out_point.x = flat_point.x + direction.x * signed_offset;
    out_point.y = flat_point.y + direction.y * signed_offset;
    out_point.z = flat_point.z + direction.z * signed_offset;
    out_point.valid = std::isfinite(out_point.x) &&
                      std::isfinite(out_point.y) &&
                      std::isfinite(out_point.z);
    return out_point.valid;
}

bool build_asymmetric_camera_ray_extrusion(const Asset& asset,
                                           const AnchorWorldPoint3& flat_point,
                                           float extrusion_backward,
                                           float extrusion_forward,
                                           AnchorWorldPoint3& out_near_point,
                                           AnchorWorldPoint3& out_far_point,
                                           AnchorWorldPoint3* out_direction) {
    if (!flat_point.valid ||
        !std::isfinite(extrusion_backward) ||
        !std::isfinite(extrusion_forward) ||
        extrusion_backward < 0.0f ||
        extrusion_forward < 0.0f) {
        return false;
    }

    AnchorWorldPoint3 direction{};
    if (!compute_camera_to_point_ray(asset, flat_point, direction)) {
        return false;
    }
    if (out_direction) {
        *out_direction = direction;
    }

    out_near_point = AnchorWorldPoint3{
        flat_point.x - direction.x * extrusion_backward,
        flat_point.y - direction.y * extrusion_backward,
        flat_point.z - direction.z * extrusion_backward,
        true};
    out_far_point = AnchorWorldPoint3{
        flat_point.x + direction.x * extrusion_forward,
        flat_point.y + direction.y * extrusion_forward,
        flat_point.z + direction.z * extrusion_forward,
        true};

    const bool valid =
        std::isfinite(out_near_point.x) &&
        std::isfinite(out_near_point.y) &&
        std::isfinite(out_near_point.z) &&
        std::isfinite(out_far_point.x) &&
        std::isfinite(out_far_point.y) &&
        std::isfinite(out_far_point.z);
    out_near_point.valid = valid;
    out_far_point.valid = valid;
    return valid;
}

bool sample_perspective_scale_at_point(const WarpedScreenGrid& cam,
                                       const anchor_points::AnchorWorldPoint3& world_point,
                                       float& out_scale) {
    out_scale = 1.0f;
    if (!world_point.valid ||
        !std::isfinite(world_point.x) ||
        !std::isfinite(world_point.y) ||
        !std::isfinite(world_point.z)) {
        return false;
    }

    const SDL_Point sample_world{
        static_cast<int>(std::lround(world_point.x)),
        static_cast<int>(std::lround(world_point.y)),
    };
    const int sample_world_z = static_cast<int>(std::lround(world_point.z));
    const WarpedScreenGrid::RenderEffects effects =
        cam.compute_render_effects(sample_world,
                                   0.0f,
                                   0.0f,
                                   WarpedScreenGrid::RenderSmoothingKey{},
                                   sample_world_z);
    if (!std::isfinite(effects.distance_scale) || effects.distance_scale <= 0.0f) {
        return false;
    }

    out_scale = std::max(0.0001f, effects.distance_scale);
    return true;
}

bool has_opposite_sign_or_zero(float lhs, float rhs) {
    return (lhs == 0.0f) || (rhs == 0.0f) || ((lhs < 0.0f) != (rhs < 0.0f));
}

bool screen_y_for_world_point(const WarpedScreenGrid& cam,
                              float world_x,
                              float world_y,
                              float world_z,
                              float& out_screen_y) {
    const render_projection::WorldPoint3 world_point{
        world_x,
        world_y,
        world_z,
        true};
    SDL_FPoint screen_point{};
    if (!render_projection::project_world_to_screen(cam, world_point, screen_point) ||
        !std::isfinite(screen_point.y)) {
        return false;
    }
    out_screen_y = screen_point.y;
    return true;
}

bool solve_anchor_with_locked_x_and_screen_y(const WarpedScreenGrid& cam,
                                             const anchor_points::AnchorWorldPoint3& flat_point,
                                             const anchor_points::AnchorWorldPoint3& ray_point,
                                             float target_screen_y,
                                             anchor_points::AnchorWorldPoint3& out_point) {
    out_point = ray_point;
    if (!flat_point.valid ||
        !ray_point.valid ||
        !std::isfinite(flat_point.x) ||
        !std::isfinite(ray_point.y) ||
        !std::isfinite(ray_point.z) ||
        !std::isfinite(target_screen_y)) {
        return false;
    }

    const float fixed_x = flat_point.x;
    const float fixed_z = ray_point.z;
    const float initial_y = ray_point.y;

    float initial_screen_y = 0.0f;
    if (!screen_y_for_world_point(cam, fixed_x, initial_y, fixed_z, initial_screen_y)) {
        return false;
    }
    const float initial_error = initial_screen_y - target_screen_y;
    if (std::fabs(initial_error) <= 0.25f) {
        out_point = anchor_points::AnchorWorldPoint3{fixed_x, initial_y, fixed_z, true};
        return true;
    }

    float span = std::max(32.0f, std::fabs(ray_point.y - flat_point.y) + 16.0f);
    float low_y = initial_y - span;
    float high_y = initial_y + span;
    float low_screen_y = 0.0f;
    float high_screen_y = 0.0f;
    float low_error = 0.0f;
    float high_error = 0.0f;
    bool bracketed = false;

    for (int i = 0; i < 12; ++i) {
        const bool low_valid = screen_y_for_world_point(cam, fixed_x, low_y, fixed_z, low_screen_y);
        const bool high_valid = screen_y_for_world_point(cam, fixed_x, high_y, fixed_z, high_screen_y);
        if (low_valid && high_valid) {
            low_error = low_screen_y - target_screen_y;
            high_error = high_screen_y - target_screen_y;
            if (has_opposite_sign_or_zero(low_error, high_error)) {
                bracketed = true;
                break;
            }
        }
        span *= 2.0f;
        low_y = initial_y - span;
        high_y = initial_y + span;
    }

    if (!bracketed) {
        return false;
    }

    float best_y = initial_y;
    float best_abs_error = std::fabs(initial_error);
    if (std::fabs(low_error) < best_abs_error) {
        best_abs_error = std::fabs(low_error);
        best_y = low_y;
    }
    if (std::fabs(high_error) < best_abs_error) {
        best_abs_error = std::fabs(high_error);
        best_y = high_y;
    }

    for (int i = 0; i < 24; ++i) {
        const float mid_y = low_y + (high_y - low_y) * 0.5f;
        float mid_screen_y = 0.0f;
        if (!screen_y_for_world_point(cam, fixed_x, mid_y, fixed_z, mid_screen_y)) {
            return false;
        }
        const float mid_error = mid_screen_y - target_screen_y;
        const float mid_abs_error = std::fabs(mid_error);
        if (mid_abs_error < best_abs_error) {
            best_abs_error = mid_abs_error;
            best_y = mid_y;
        }
        if (mid_abs_error <= 0.25f) {
            break;
        }

        if (has_opposite_sign_or_zero(low_error, mid_error)) {
            high_y = mid_y;
            high_error = mid_error;
        } else {
            low_y = mid_y;
            low_error = mid_error;
        }
    }

    out_point = anchor_points::AnchorWorldPoint3{
        fixed_x,
        best_y,
        fixed_z,
        std::isfinite(fixed_x) && std::isfinite(best_y) && std::isfinite(fixed_z)};
    return out_point.valid;
}

FrameAnchorSample resolve_frame_anchor_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    FrameAnchorSample sample{};
    sample.resolved.depth_offset = anchor.depth_offset;

    Assets* assets_owner = asset.get_assets();
    if (!assets_owner) {
        sample.resolved.missing = true;
        return sample;
    }

    FrameDimensions dims{};
    if (!gather_frame_dimensions(asset, dims)) {
        sample.resolved.missing = true;
        return sample;
    }

    const AnchorFrameSample anchor_sample = compute_anchor_frame_sample(asset, anchor, dims);

    const WarpedScreenGrid& cam = assets_owner->getView();
    const Asset::PerspectiveSample perspective_sample = asset.runtime_perspective_sample();
    const float perspective_scale = perspective_sample.scale;
    const int resolution_layer = perspective_sample.resolution_layer;

    const world::GridPoint* render_gp = cam.grid_point_for_asset(&asset);
    const bool has_render_perspective =
        render_gp &&
        std::isfinite(render_gp->perspective_scale()) &&
        render_gp->perspective_scale() > 0.0f;
    // Anchor projection must use the exact same perspective sample the render pass uses.
    const float render_perspective = has_render_perspective
        ? std::max(0.0001f, render_gp->perspective_scale())
        : perspective_scale;
    const int render_resolution_layer =
        (has_render_perspective && render_gp) ? render_gp->resolution_layer() : resolution_layer;
    const float perspective_delta = std::fabs(render_perspective - perspective_scale);
    const std::uint64_t camera_state_version = cam.camera_state_version();
    const bool expected_override_divergence =
        perspective_sample.source == Asset::PerspectiveSource::AnchorBindingOverride;
    if (has_render_perspective &&
        !expected_override_divergence &&
        perspective_delta > 1e-3f &&
        should_warn_perspective_divergence(asset, camera_state_version)) {
        vibble::log::warn(std::string("[AnchorResolver] perspective mismatch for asset '") +
                          (asset.info ? asset.info->name : std::string{"<unknown>"}) +
                          "' resolver=" + std::to_string(perspective_scale) +
                          " render=" + std::to_string(render_perspective) +
                          " delta=" + std::to_string(perspective_delta) +
                          " source=" + Asset::perspective_source_label(perspective_sample.source));
    }
    if (should_trace_anchor_resolver(asset) &&
        should_emit_anchor_trace_for_camera_state(asset, camera_state_version)) {
        vibble::log::debug(std::string("[ScaleTrace][Anchor] asset='") +
                           (asset.info ? asset.info->name : std::string{"<unknown>"}) +
                           "' camera_state=" + std::to_string(camera_state_version) +
                           " source=" + Asset::perspective_source_label(perspective_sample.source) +
                           " resolver=" + std::to_string(perspective_scale) +
                           " render=" + std::to_string(render_perspective) +
                           " delta=" + std::to_string(perspective_delta));
    }

    render_projection::SpriteProjectionInput projection_input{};
    projection_input.world_x = asset.smoothed_translation_x();
    projection_input.world_y = asset.smoothed_translation_y();
    projection_input.world_z = static_cast<float>(asset.world_z()) + dims.world_z_offset;
    projection_input.perspective_scale = render_perspective;
    projection_input.frame_width_px = dims.frame_w;
    projection_input.frame_height_px = dims.frame_h;
    projection_input.final_width_px = dims.final_w;
    projection_input.final_height_px = dims.final_h;
    projection_input.flip = dims.flip;
    projection_input.angle = dims.angle;

    render_projection::ProjectedSpriteFrame projected_frame{};
    if (!render_projection::build_projected_sprite_frame(cam, projection_input, projected_frame)) {
        sample.resolved.missing = true;
        return sample;
    }

    sample.uv = projected_frame.anchor_uv_from_texture_pixel(anchor_sample.scaled_px);
    const SDL_FPoint flat_texture_screen_px = projected_frame.sample_screen_from_uv(sample.uv);
    if (!std::isfinite(flat_texture_screen_px.x) || !std::isfinite(flat_texture_screen_px.y)) {
        sample.resolved.missing = true;
        return sample;
    }

    render_projection::WorldPoint3 flat_relative_pixel_point{};
    if (!cam.screen_to_world_on_depth_plane(flat_texture_screen_px,
                                            projection_input.world_z,
                                            flat_relative_pixel_point)) {
        sample.resolved.missing = true;
        return sample;
    }
    sample.flat_screen_px = flat_texture_screen_px;
    sample.has_flat_screen_px = true;
    sample.flat_relative_pixel_point = AnchorWorldPoint3{
        flat_relative_pixel_point.x,
        flat_relative_pixel_point.y,
        flat_relative_pixel_point.z,
        true};
    sample.resolved.flat_world_exact_pos_2d = Vec2{
        sample.flat_relative_pixel_point.x,
        sample.flat_relative_pixel_point.y};
    sample.resolved.flat_world_exact_z = sample.flat_relative_pixel_point.z;
    const bool has_parent_perspective_sample =
        perspective_sample.source != Asset::PerspectiveSource::Default &&
        std::isfinite(perspective_scale) &&
        perspective_scale > 0.0f;
    const float parent_perspective_sample =
        has_parent_perspective_sample ? std::max(0.0001f, perspective_scale) : 1.0f;

    if (!displace_along_camera_to_point_ray(asset,
                                            sample.flat_relative_pixel_point,
                                            static_cast<float>(anchor.depth_offset),
                                            sample.final_anchor_point)) {
        sample.resolved.missing = true;
        return sample;
    }

    const anchor_points::AnchorWorldPoint3 ray_displaced_anchor_point = sample.final_anchor_point;
    if (!anchor.resolve_x) {
        anchor_points::AnchorWorldPoint3 no_resolve_x_point{};
        if (solve_anchor_with_locked_x_and_screen_y(cam,
                                                    sample.flat_relative_pixel_point,
                                                    ray_displaced_anchor_point,
                                                    sample.flat_screen_px.y,
                                                    no_resolve_x_point)) {
            sample.final_anchor_point = no_resolve_x_point;
        } else {
            // Keep current behavior on solver failure.
            sample.final_anchor_point = ray_displaced_anchor_point;
        }
    }

    render_projection::WorldPoint3 final_anchor_point{
        sample.final_anchor_point.x,
        sample.final_anchor_point.y,
        sample.final_anchor_point.z,
        true};

    auto apply_perspective_override = [&](float scale) {
        sample.resolved.flat_perspective_scale = std::max(0.0001f, scale);
        sample.resolved.has_flat_perspective_scale = true;
    };

    float selected_method_scale = 1.0f;
    bool has_selected_method_scale = false;
    switch (anchor.scaling_method) {
        case AnchorScalingMethod::Parent:
            break;
        case AnchorScalingMethod::Real3DPoint:
            has_selected_method_scale =
                sample_perspective_scale_at_point(cam, sample.final_anchor_point, selected_method_scale);
            break;
        case AnchorScalingMethod::Relative2DAnchorPoint:
            has_selected_method_scale =
                sample_perspective_scale_at_point(cam, sample.flat_relative_pixel_point, selected_method_scale);
            break;
        case AnchorScalingMethod::Real3DFloorPoint: {
            anchor_points::AnchorWorldPoint3 floor_point = sample.final_anchor_point;
            floor_point.y = 0.0f;
            floor_point.valid = std::isfinite(floor_point.x) &&
                                std::isfinite(floor_point.y) &&
                                std::isfinite(floor_point.z);
            has_selected_method_scale =
                sample_perspective_scale_at_point(cam, floor_point, selected_method_scale);
            break;
        }
    }

    if (anchor.scaling_method == AnchorScalingMethod::Parent) {
        if (has_parent_perspective_sample) {
            apply_perspective_override(parent_perspective_sample);
        }
    } else if (has_selected_method_scale) {
        apply_perspective_override(selected_method_scale);
    } else if (has_parent_perspective_sample) {
        apply_perspective_override(parent_perspective_sample);
    }

    const int resolved_x = static_cast<int>(std::lround(final_anchor_point.x));
    const int resolved_y = static_cast<int>(std::lround(final_anchor_point.y));
    const int resolved_z = static_cast<int>(std::lround(final_anchor_point.z));

    sample.resolved.world_exact_pos_2d = Vec2{final_anchor_point.x, final_anchor_point.y};
    sample.resolved.world_exact_z = final_anchor_point.z;
    sample.resolved.world_px = SDL_Point{resolved_x, resolved_y};
    sample.resolved.world_z = resolved_z;
    sample.resolved.world_depth = final_anchor_point.z;
    sample.resolved.resolution_layer = render_resolution_layer;
    sample.resolved.source_texture_px = anchor_sample.source_px;
    sample.resolved.has_canonical_texture_source = true;
    sample.resolved.missing = false;

    world::WorldGrid& grid = assets_owner->world_grid();
    const world::GridKey key{resolved_x, resolved_y, resolved_z, render_resolution_layer};
    if (grid_policy == GridMaterialization::Ensure) {
        sample.resolved.grid_point = &grid.find_or_create_grid_point(key);
    } else {
        sample.resolved.grid_point = grid.find_grid_point_strict(key);
    }

    SDL_FPoint final_screen_px{};
    if (render_projection::project_world_to_screen(cam, final_anchor_point, final_screen_px)) {
        sample.final_screen_px = final_screen_px;
        sample.has_final_screen_px = true;
    }
    sample.screen_px = sample.has_final_screen_px ? sample.final_screen_px : sample.flat_screen_px;
    return sample;
}

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy) {
    const auto sample = resolve_frame_anchor_sample(asset, anchor, grid_policy);
    return PixelLockedAnchor{sample.resolved, sample.screen_px};
}

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy) {
    return resolve_frame_anchor_sample(asset, anchor, grid_policy).resolved;
}

}

#endif
