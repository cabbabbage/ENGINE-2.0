#include "rendering/render/projected_sprite_frame.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

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
    const double safe_w = static_cast<double>(std::max(1, params.screen_width));
    const double safe_h = static_cast<double>(std::max(1, params.screen_height));
    const double ndc_x_scaled = (screen_x / safe_w) * 2.0 - 1.0;
    const double ndc_y_scaled = 1.0 - ((screen_y - params.screen_pan_y_px) / safe_h) * 2.0;
    const double inv_zoom = (params.screen_zoom > 0.0 && std::isfinite(params.screen_zoom))
        ? (1.0 / params.screen_zoom)
        : 1.0;
    return std::pair<double, double>{ndc_x_scaled * inv_zoom, ndc_y_scaled * inv_zoom};
}

}  // namespace

namespace render_projection {

SDL_FPoint ProjectedSpriteFrame::anchor_uv_from_texture_pixel(SDL_Point texture_px) const {
    auto to_unit = [](int pixel, int dimension) {
        if (dimension <= 0) {
            return 0.5f;
        }
        return std::clamp((static_cast<float>(pixel) + 0.5f) / static_cast<float>(dimension), 0.0f, 1.0f);
    };

    const int safe_w = std::max(1, frame_width_px);
    const int safe_h = std::max(1, frame_height_px);
    const int clamped_x = std::clamp(texture_px.x, 0, safe_w - 1);
    const int clamped_y = std::clamp(texture_px.y, 0, safe_h - 1);

    const float u = to_unit(clamped_x, safe_w);
    const float v = to_unit(clamped_y, safe_h);
    const bool flip_h = (flip & SDL_FLIP_HORIZONTAL) != 0;
    return SDL_FPoint{flip_h ? (1.0f - u) : u, v};
}

SDL_FPoint ProjectedSpriteFrame::sample_screen_from_uv(SDL_FPoint uv) const {
    uv.x = std::clamp(uv.x, 0.0f, 1.0f);
    uv.y = std::clamp(uv.y, 0.0f, 1.0f);

    const SDL_FPoint top{
        screen_tl.x + (screen_tr.x - screen_tl.x) * uv.x,
        screen_tl.y + (screen_tr.y - screen_tl.y) * uv.x};
    const SDL_FPoint bottom{
        screen_bl.x + (screen_br.x - screen_bl.x) * uv.x,
        screen_bl.y + (screen_br.y - screen_bl.y) * uv.x};
    return SDL_FPoint{
        top.x + (bottom.x - top.x) * uv.y,
        top.y + (bottom.y - top.y) * uv.y};
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
    const float world_width = static_cast<float>(input.final_width_px) / safe_perspective;
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

    const float aspect = static_cast<float>(input.frame_height_px) / static_cast<float>(input.frame_width_px);
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

