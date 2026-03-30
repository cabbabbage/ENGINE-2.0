#include "rendering/render/projected_sprite_frame.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "rendering/render/screen_space_math.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace {

bool project_world_point(const WarpedScreenGrid& cam,
                         float world_x,
                         float world_y,
                         float world_z,
                         SDL_FPoint& out_screen) {
    if (!cam.project_world_point(SDL_FPoint{world_x, world_y}, world_z, out_screen)) {
        return false;
    }
    return std::isfinite(out_screen.x) && std::isfinite(out_screen.y);
}

std::pair<double, double> screen_to_ndc_point(const world::CameraProjectionParams& params,
                                               double screen_x,
                                               double screen_y) {
    return render::screen_space::screen_to_ndc(screen_x,
                                               screen_y,
                                               params.screen_width,
                                               params.screen_height,
                                               params.screen_zoom,
                                               params.screen_pan_y_px);
}

SDL_FPoint sanitize_anchor_uv(SDL_FPoint uv) {
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        return SDL_FPoint{0.5f, 1.0f};
    }
    return SDL_FPoint{
        std::clamp(uv.x, 0.0f, 1.0f),
        std::clamp(uv.y, 0.0f, 1.0f)};
}

SDL_FPoint rotate_about_point(const SDL_FPoint& point,
                              const SDL_FPoint& pivot,
                              float radians) {
    const float dx = point.x - pivot.x;
    const float dy = point.y - pivot.y;
    const float cos_theta = std::cos(radians);
    const float sin_theta = std::sin(radians);
    return SDL_FPoint{
        pivot.x + (dx * cos_theta - dy * sin_theta),
        pivot.y + (dx * sin_theta + dy * cos_theta)};
}

bool calibrate_pixels_per_world_x(const WarpedScreenGrid& cam,
                                  const render_projection::SpriteProjectionInput& input,
                                  float& out_pixels_per_world_x) {
    out_pixels_per_world_x = 0.0f;
    constexpr float kHalfWorldSpan = 0.5f;

    SDL_FPoint left_screen{};
    SDL_FPoint right_screen{};
    if (!project_world_point(cam,
                             input.world_x - kHalfWorldSpan,
                             input.world_y,
                             input.world_z,
                             left_screen) ||
        !project_world_point(cam,
                             input.world_x + kHalfWorldSpan,
                             input.world_y,
                             input.world_z,
                             right_screen)) {
        return false;
    }

    const float dx = right_screen.x - left_screen.x;
    const float dy = right_screen.y - left_screen.y;
    const float pixel_span = std::hypot(dx, dy);
    if (!std::isfinite(pixel_span) || pixel_span <= 1e-5f) {
        return false;
    }

    out_pixels_per_world_x = pixel_span; // Sampled over exactly one world unit.
    return true;
}

bool compute_legacy_anchor_screen_point(const WarpedScreenGrid& cam,
                                        const render_projection::SpriteProjectionInput& input,
                                        float safe_perspective,
                                        float final_width_px,
                                        SDL_FPoint& out_anchor) {
    out_anchor = SDL_FPoint{};
    if (!std::isfinite(safe_perspective) || safe_perspective <= 0.0f ||
        !std::isfinite(final_width_px) || final_width_px <= 0.0f) {
        return false;
    }

    const float legacy_world_width = final_width_px / safe_perspective;
    const float legacy_half_width = legacy_world_width * 0.5f;
    if (!std::isfinite(legacy_half_width) || legacy_half_width <= 0.0f) {
        return false;
    }

    SDL_FPoint legacy_bl{};
    SDL_FPoint legacy_br{};
    if (!project_world_point(cam,
                             input.world_x - legacy_half_width,
                             input.world_y,
                             input.world_z,
                             legacy_bl) ||
        !project_world_point(cam,
                             input.world_x + legacy_half_width,
                             input.world_y,
                             input.world_z,
                             legacy_br)) {
        return false;
    }

    out_anchor = SDL_FPoint{
        0.5f * (legacy_bl.x + legacy_br.x),
        0.5f * (legacy_bl.y + legacy_br.y)};
    return std::isfinite(out_anchor.x) && std::isfinite(out_anchor.y);
}

}  // namespace

namespace render_projection {

SDL_FPoint ProjectedSpriteFrame::anchor_uv_from_texture_pixel(SDL_Point texture_px) const {
    auto to_unit = [](int pixel, int dimension) {
        if (dimension <= 0) {
            return 0.5f;
        }
        return (static_cast<float>(pixel) + 0.5f) / static_cast<float>(dimension);
    };

    const int safe_w = std::max(1, frame_width_px);
    const int safe_h = std::max(1, frame_height_px);
    const float u = to_unit(texture_px.x, safe_w);
    const float v = to_unit(texture_px.y, safe_h);
    const bool flip_h = (flip & SDL_FLIP_HORIZONTAL) != 0;
    return SDL_FPoint{flip_h ? (1.0f - u) : u, v};
}

SDL_FPoint ProjectedSpriteFrame::sample_screen_from_uv(SDL_FPoint uv) const {
    // Match SDL_RenderGeometry's fixed triangle split: (TL,TR,BR) and (TL,BR,BL).
    if (uv.x >= uv.y) {
        const float w_tl = 1.0f - uv.x;
        const float w_tr = uv.x - uv.y;
        const float w_br = uv.y;
        return SDL_FPoint{
            screen_tl.x * w_tl + screen_tr.x * w_tr + screen_br.x * w_br,
            screen_tl.y * w_tl + screen_tr.y * w_tr + screen_br.y * w_br};
    }

    const float w_tl = 1.0f - uv.y;
    const float w_bl = uv.y - uv.x;
    const float w_br = uv.x;
    return SDL_FPoint{
        screen_tl.x * w_tl + screen_bl.x * w_bl + screen_br.x * w_br,
        screen_tl.y * w_tl + screen_bl.y * w_bl + screen_br.y * w_br};
}

bool build_projected_sprite_frame(const WarpedScreenGrid& cam,
                                  const SpriteProjectionInput& input,
                                  ProjectedSpriteFrame& out) {
    out = ProjectedSpriteFrame{};
    if (input.final_width_px <= 0 || input.final_height_px <= 0 ||
        input.frame_width_px <= 0 || input.frame_height_px <= 0) {
        return false;
    }

    const float safe_perspective =
        (std::isfinite(input.perspective_scale) && input.perspective_scale > 0.0f)
            ? input.perspective_scale
            : 1.0f;
    const float final_width_px = static_cast<float>(input.final_width_px);
    const float final_height_px = static_cast<float>(input.final_height_px);
    if (!std::isfinite(final_width_px) || !std::isfinite(final_height_px) ||
        final_width_px <= 0.0f || final_height_px <= 0.0f) {
        return false;
    }

    float pixels_per_world_x = 0.0f;
    const bool has_local_calibration =
        calibrate_pixels_per_world_x(cam, input, pixels_per_world_x);
    const float world_width =
        has_local_calibration
            ? (final_width_px / pixels_per_world_x)
            : (final_width_px / safe_perspective);
    const float half_width = world_width * 0.5f;
    if (!std::isfinite(half_width) || half_width <= 0.0f) {
        return false;
    }

    SDL_FPoint screen_bl{};
    SDL_FPoint screen_br{};
    if (!project_world_point(cam, input.world_x - half_width, input.world_y, input.world_z, screen_bl) ||
        !project_world_point(cam, input.world_x + half_width, input.world_y, input.world_z, screen_br)) {
        return false;
    }

    const float bottom_dx = screen_br.x - screen_bl.x;
    const float bottom_dy = screen_br.y - screen_bl.y;
    const float bottom_len = std::hypot(bottom_dx, bottom_dy);
    if (bottom_len < 1e-5f || !std::isfinite(bottom_len)) {
        return false;
    }

    const float aspect = final_height_px / final_width_px;
    float screen_height = bottom_len * aspect;
    if (!std::isfinite(screen_height) || screen_height <= 0.0f) {
        return false;
    }

    const float nx = -bottom_dy / bottom_len;
    const float ny = bottom_dx / bottom_len;

    const SDL_FPoint cand_tl_a{screen_bl.x + nx * screen_height, screen_bl.y + ny * screen_height};
    const SDL_FPoint cand_tr_a{screen_br.x + nx * screen_height, screen_br.y + ny * screen_height};
    const SDL_FPoint cand_tl_b{screen_bl.x - nx * screen_height, screen_bl.y - ny * screen_height};
    const SDL_FPoint cand_tr_b{screen_br.x - nx * screen_height, screen_br.y - ny * screen_height};

    const float avg_bottom_y = 0.5f * (screen_bl.y + screen_br.y);
    const float avg_top_a = 0.5f * (cand_tl_a.y + cand_tr_a.y);
    const float avg_top_b = 0.5f * (cand_tl_b.y + cand_tr_b.y);

    SDL_FPoint screen_tl{};
    SDL_FPoint screen_tr{};
    const bool a_is_above = avg_top_a < avg_bottom_y;
    const bool b_is_above = avg_top_b < avg_bottom_y;
    if (a_is_above && (!b_is_above || avg_top_a <= avg_top_b)) {
        screen_tl = cand_tl_a;
        screen_tr = cand_tr_a;
    } else if (b_is_above && (!a_is_above || avg_top_b <= avg_top_a)) {
        screen_tl = cand_tl_b;
        screen_tr = cand_tr_b;
    } else if (avg_top_a <= avg_top_b) {
        screen_tl = cand_tl_a;
        screen_tr = cand_tr_a;
    } else {
        screen_tl = cand_tl_b;
        screen_tr = cand_tr_b;
    }

    // Re-anchor the projected quad so caller-specified UV maps to the same
    // world anchor point used by the default bottom-center projection.
    ProjectedSpriteFrame anchor_sample_frame{};
    anchor_sample_frame.screen_tl = screen_tl;
    anchor_sample_frame.screen_tr = screen_tr;
    anchor_sample_frame.screen_br = screen_br;
    anchor_sample_frame.screen_bl = screen_bl;

    SDL_FPoint desired_anchor{
        0.5f * (screen_bl.x + screen_br.x),
        0.5f * (screen_bl.y + screen_br.y)};
    SDL_FPoint legacy_anchor{};
    if (compute_legacy_anchor_screen_point(cam,
                                           input,
                                           safe_perspective,
                                           final_width_px,
                                           legacy_anchor)) {
        desired_anchor = legacy_anchor;
    }
    const SDL_FPoint current_anchor =
        anchor_sample_frame.sample_screen_from_uv(sanitize_anchor_uv(input.anchor_uv));
    if (std::isfinite(current_anchor.x) && std::isfinite(current_anchor.y)) {
        const float offset_x = desired_anchor.x - current_anchor.x;
        const float offset_y = desired_anchor.y - current_anchor.y;
        screen_tl.x += offset_x;
        screen_tl.y += offset_y;
        screen_tr.x += offset_x;
        screen_tr.y += offset_y;
        screen_br.x += offset_x;
        screen_br.y += offset_y;
        screen_bl.x += offset_x;
        screen_bl.y += offset_y;
    }

    const double angle_degrees = std::isfinite(input.angle) ? input.angle : 0.0;
    if (std::fabs(angle_degrees) > 1e-6) {
        constexpr float kPi = 3.14159265358979323846f;
        const float radians = static_cast<float>(angle_degrees) * (kPi / 180.0f);
        screen_tl = rotate_about_point(screen_tl, desired_anchor, radians);
        screen_tr = rotate_about_point(screen_tr, desired_anchor, radians);
        screen_br = rotate_about_point(screen_br, desired_anchor, radians);
        screen_bl = rotate_about_point(screen_bl, desired_anchor, radians);
    }

    out.screen_tl = screen_tl;
    out.screen_tr = screen_tr;
    out.screen_br = screen_br;
    out.screen_bl = screen_bl;
    out.frame_width_px = input.frame_width_px;
    out.frame_height_px = input.frame_height_px;
    out.final_width_px = input.final_width_px;
    out.final_height_px = input.final_height_px;
    out.flip = input.flip;
    out.world_z = input.world_z;
    out.perspective_scale = safe_perspective;
    out.valid = true;
    return true;
}

bool build_camera_ray_from_screen(const world::CameraProjectionParams& params,
                                  const SDL_FPoint& screen_point,
                                  CameraRay& out_ray) {
    out_ray = CameraRay{};
    const double safe_scale = std::max(1e-6, params.meters_scale);
    if (!std::isfinite(safe_scale)) {
        return false;
    }

    const auto [ndc_x, ndc_y] = screen_to_ndc_point(
        params,
        static_cast<double>(screen_point.x),
        static_cast<double>(screen_point.y));
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
        return false;
    }

    const double dir_x = params.forward_x + params.right_x * (ndc_x * params.tan_half_fov_x) +
                         params.up_x * (ndc_y * params.tan_half_fov_y);
    const double dir_y = params.forward_y + params.right_y * (ndc_x * params.tan_half_fov_x) +
                         params.up_y * (ndc_y * params.tan_half_fov_y);
    const double dir_z = params.forward_z + params.right_z * (ndc_x * params.tan_half_fov_x) +
                         params.up_z * (ndc_y * params.tan_half_fov_y);
    const double dir_len = std::sqrt(dir_x * dir_x + dir_y * dir_y + dir_z * dir_z);
    if (!std::isfinite(dir_len) || dir_len <= 1e-8) {
        return false;
    }

    const double inv_len = 1.0 / dir_len;
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
                                     WorldPoint3& out_world_point) {
    out_world_point = WorldPoint3{};
    if (!ray.valid) {
        return false;
    }

    const double safe_scale = std::max(1e-6, params.meters_scale);
    if (!std::isfinite(safe_scale)) {
        return false;
    }

    if (std::abs(static_cast<double>(ray.direction.z)) <= 1e-8) {
        return false;
    }

    const double target_z_meters = (static_cast<double>(target_world_z) - params.anchor_world_z) * safe_scale;
    const double t = (target_z_meters - params.position_z) / static_cast<double>(ray.direction.z);
    if (!std::isfinite(t) || t <= 0.0) {
        return false;
    }

    const double hit_x_m = params.position_x + static_cast<double>(ray.direction.x) * t;
    const double hit_y_m = params.position_y + static_cast<double>(ray.direction.y) * t;

    out_world_point.x = static_cast<float>(hit_x_m / safe_scale + params.anchor_world_x);
    out_world_point.y = static_cast<float>(hit_y_m / safe_scale + params.anchor_world_y);
    out_world_point.z = target_world_z;
    out_world_point.valid =
        std::isfinite(out_world_point.x) &&
        std::isfinite(out_world_point.y) &&
        std::isfinite(out_world_point.z);
    return out_world_point.valid;
}

bool project_world_to_screen(const WarpedScreenGrid& cam,
                             const WorldPoint3& world_point,
                             SDL_FPoint& out_screen) {
    if (!world_point.valid) {
        return false;
    }
    if (!cam.project_world_point(SDL_FPoint{world_point.x, world_point.y}, world_point.z, out_screen)) {
        return false;
    }
    return std::isfinite(out_screen.x) && std::isfinite(out_screen.y);
}

}  // namespace render_projection
