#include "warped_screen_grid.hpp"

#include "asset/Asset.hpp"
#include "utils/area.hpp"
#include "map_generation/room.hpp"
#include "core/find_current_room.hpp"
#include "utils/log.hpp"
#include "world/world_grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>
#include <tuple>
#include <string>
#include <limits>
#include <optional>
#include <nlohmann/json.hpp>

template <typename T>
T lerp(T a, T b, double t) {
    return static_cast<T>(a + (b - a) * t);
}

namespace {
    constexpr double PI_D       = 3.14159265358979323846;
    constexpr double kHalfFovY  = PI_D / 4.0;
    constexpr float  kVirtualScreenScale = 1.5f;
    struct CameraState;

    static inline Area make_rect_area(const std::string& name, SDL_Point center, int w, int h, int resolution) {
        const int left   = center.x - (w / 2);
        const int top    = center.y - (h / 2);
        const int right  = left + w;
        const int bottom = top + h;
        std::vector<Area::Point> corners{
            { left,  top    },
            { right, top    },
            { right, bottom },
            { left,  bottom }
        };
        return Area(name, corners, resolution);
    }

    // clamp_height_scale and all scale-based logic removed; camera is now fully explicit per-room.

    struct Vec3 {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    Vec3 operator+(const Vec3& a, const Vec3& b) {
        return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z };
    }

    Vec3 operator-(const Vec3& a, const Vec3& b) {
        return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
    }

    Vec3 operator*(const Vec3& v, double s) {
        return Vec3{ v.x * s, v.y * s, v.z * s };
    }

    double dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Vec3 cross(const Vec3& a, const Vec3& b) {
        return Vec3{
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    double length(const Vec3& v) {
        return std::sqrt(dot(v, v));
    }

    Vec3 normalize(const Vec3& v) {
        const double len = length(v);
        if (len <= 1e-6 || !std::isfinite(len)) {
            return Vec3{0.0, 0.0, 0.0};
        }
        const double inv = 1.0 / len;
        return Vec3{ v.x * inv, v.y * inv, v.z * inv };
    }

    struct CameraState {
        bool   valid = false;
        Vec3   position{};
        Vec3   forward{};
        Vec3   right{};
        Vec3   up{};
        SDL_FPoint anchor_world_px{0.0f, 0.0f};
        double camera_height = 0.0;
        double focus_depth   = 0.0;
        double reference_depth = 1.0;
        double tan_half_fov_y = std::tan(kHalfFovY);
        double tan_half_fov_x = std::tan(kHalfFovY);
        double near_plane = 0.0;
        double far_plane  = 0.0;
        double horizon_screen_y = 0.0;
        double meters_scale = 1.0;
        double pitch_radians = 0.0;
        float  pitch_degrees = 0.0f;
        double camera_world_y = 0.0;
        double anchor_world_y = 0.0;
        double focus_ndc_offset = 0.0;
        double screen_zoom = 1.0;
        double inv_screen_zoom = 1.0;
        double screen_pan_y_px = 0.0;
        float  near_camera_max_perspective_scale = 0.0f;
        float  offscreen_fade_amount_px = 0.0f;
    };

    SDL_FPoint ndc_to_screen_point(const CameraState& cam,
                                   double ndc_x,
                                   double ndc_y,
                                   int screen_width,
                                   int screen_height) {
        const double scaled_x = ndc_x * cam.screen_zoom;
        const double scaled_y = ndc_y * cam.screen_zoom;
        const double screen_x = (scaled_x * 0.5 + 0.5) * static_cast<double>(screen_width);
        const double screen_y = (0.5 - scaled_y * 0.5) * static_cast<double>(screen_height) + cam.screen_pan_y_px;
        return SDL_FPoint{
            static_cast<float>(screen_x),
            static_cast<float>(screen_y)
        };
    }

    std::pair<double, double> screen_to_ndc_point(const CameraState& cam,
                                                  double screen_x,
                                                  double screen_y,
                                                  int screen_width,
                                                  int screen_height) {
        const double safe_w = static_cast<double>(std::max(1, screen_width));
        const double safe_h = static_cast<double>(std::max(1, screen_height));
        const double ndc_x_scaled = (screen_x / safe_w) * 2.0 - 1.0;
        const double ndc_y_scaled = 1.0 - ((screen_y - cam.screen_pan_y_px) / safe_h) * 2.0;
        const double inv_zoom = (cam.inv_screen_zoom > 0.0 && std::isfinite(cam.inv_screen_zoom))
            ? cam.inv_screen_zoom
            : 1.0;
        return std::pair<double, double>{ ndc_x_scaled * inv_zoom, ndc_y_scaled * inv_zoom };
    }

    struct ScreenBounds {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };

    ScreenBounds expanded_screen_bounds(int screen_width, int screen_height) {
        const float safe_w = static_cast<float>(std::max(1, screen_width));
        const float safe_h = static_cast<float>(std::max(1, screen_height));
        const float extra_w = (kVirtualScreenScale - 1.0f) * safe_w * 0.5f;
        const float extra_h = (kVirtualScreenScale - 1.0f) * safe_h * 0.5f;
        return ScreenBounds{
            -extra_w,
            -extra_h,
            safe_w + extra_w,
            safe_h + extra_h
        };
    }

    float horizon_fade_for_height(double camera_height) {
        const double safe_height = std::max(1.0, camera_height);
        const double scaled = std::clamp(std::sqrt(safe_height) * 18.0, 60.0, 420.0);
        return static_cast<float>(scaled);
    }

CameraState build_camera_state(const WarpedScreenGrid::RealismSettings& settings,
                                   double aspect,
                                   int screen_width,
                                   int screen_height,
                                   SDL_FPoint anchor_world,
                                   const CameraParams& params) {
        CameraState state{};

        const CameraParams safe_params =
            camera_math::sanitize_camera_params(params, settings.base_height_px);

        const double camera_height_pixels = safe_params.height_px;
        const float  tilt_deg             = camera_math::sanitize_pitch_degrees(static_cast<float>(safe_params.tilt_deg));
        const double y_distance_pixels    = safe_params.y_distance_px;

        const double pitch_rad = static_cast<double>(tilt_deg) * PI_D / 180.0;
        const double meters_scale_raw = std::max(1e-6, static_cast<double>(settings.meters_per_100_world_px));
        const double meters_scale = meters_scale_raw / 100.0;
        const Vec3 anchor{ 0.0, 0.0, 0.0 };
        const Vec3 camera_pos{
            0.0,
            y_distance_pixels * meters_scale,
            camera_height_pixels * meters_scale
        };
        state.screen_zoom = 1.0 + std::clamp(safe_params.zoom_percent, 0.0, 100.0) * 0.01;
        if (!std::isfinite(state.screen_zoom) || state.screen_zoom <= 0.0) {
            state.screen_zoom = 1.0;
        }
        state.inv_screen_zoom = 1.0 / state.screen_zoom;

        state.near_camera_max_perspective_scale = settings.near_camera_max_perspective_scale;
        state.offscreen_fade_amount_px = settings.offscreen_fade_amount_px;

        Vec3 to_anchor = anchor - camera_pos;
        Vec3 horiz_dir{ to_anchor.x, to_anchor.y, 0.0 };
        const double horiz_len = length(horiz_dir);
        if (horiz_len < 1e-3 || !std::isfinite(horiz_len)) {
            horiz_dir = Vec3{0.0, -1.0, 0.0};
        } else {
            horiz_dir = horiz_dir * (1.0 / horiz_len);
        }

        const double cos_pitch = std::cos(pitch_rad);
        const double sin_pitch = std::sin(pitch_rad);
        Vec3 forward{
            horiz_dir.x * cos_pitch,
            horiz_dir.y * cos_pitch,
            -sin_pitch
        };
        forward = normalize(forward);
        if (length(forward) < 1e-6 || !std::isfinite(length(forward))) {
            forward = Vec3{0.0, -cos_pitch, -sin_pitch};
            forward = normalize(forward);
        }

        Vec3 world_up{0.0, 0.0, 1.0};
        Vec3 right = cross(world_up, forward);
        if (length(right) < 1e-6 || !std::isfinite(length(right))) {
            world_up = Vec3{0.0, 1.0, 0.0};
            right = cross(world_up, forward);
        }
        right = normalize(right);
        Vec3 up = normalize(cross(forward, right));

        const double dist_horiz = length(Vec3{ anchor.x - camera_pos.x, anchor.y - camera_pos.y, 0.0 });

        state.position = camera_pos;
        state.forward = forward;
        state.right = right;
        state.up = up;
        state.anchor_world_px = anchor_world;
        state.camera_height = camera_height_pixels;
        state.focus_depth = std::max(1e-4, dist_horiz);
        state.reference_depth = std::max(1.0, state.focus_depth);
        state.valid = std::isfinite(camera_height_pixels) && std::isfinite(pitch_rad) &&
                      std::isfinite(dist_horiz) && length(forward) > 1e-6 && length(up) > 1e-6 &&
                      length(right) > 1e-6;
        state.near_plane = 0.1;
        state.far_plane  = 10000.0;
        state.tan_half_fov_y = std::tan(kHalfFovY);
        state.tan_half_fov_x = aspect * state.tan_half_fov_y;
        const double tan_pitch = std::tan(pitch_rad);
        const double horizon_ndc = (std::isfinite(tan_pitch) && std::isfinite(state.tan_half_fov_y))
            ? (tan_pitch / state.tan_half_fov_y)
            : 0.0;
        state.focus_ndc_offset = 0.0;
        const SDL_FPoint horizon_screen = ndc_to_screen_point(state, 0.0, horizon_ndc, screen_width, screen_height);
        state.horizon_screen_y = horizon_screen.y;
        state.meters_scale = meters_scale;
        state.pitch_radians = pitch_rad;
        state.pitch_degrees = tilt_deg;
        state.camera_world_y = anchor_world.y + y_distance_pixels;
        state.anchor_world_y = anchor_world.y;

        return state;
    }

struct ProjectionResult {
        bool        valid = false;
        SDL_FPoint  screen{0.0f, 0.0f};
        float       perspective_scale = 1.0f;
        float       vertical_scale    = 1.0f;
        float       horizon_fade      = 1.0f;
        float       near_camera_fade  = 1.0f;
        float       distance          = 0.0f;
        float       forward_depth     = 0.0f;
    };

    std::optional<SDL_FPoint> project_camera_space_point(const CameraState& cam,
                                                         double cam_x,
                                                         double cam_y,
                                                         double depth,
                                                         int screen_width,
                                                         int screen_height) {
        if (!std::isfinite(depth) || depth <= cam.near_plane) {
            return std::nullopt;
        }
        const double ndc_x = (cam_x / depth) / cam.tan_half_fov_x;
        const double ndc_y = (cam_y / depth) / cam.tan_half_fov_y;
        if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
            return std::nullopt;
        }
        return ndc_to_screen_point(cam, ndc_x, ndc_y, screen_width, screen_height);
    }

    float compute_perspective_scale_from_depth(const CameraState& cam,
                                               double depth_along_forward,
                                               int screen_width) {
        if (!std::isfinite(depth_along_forward) || depth_along_forward <= cam.near_plane) {
            return 1.0f;
        }
        const double safe_width = static_cast<double>(std::max(1, screen_width));
        const double tan_half_fov_x = std::max(1e-6, cam.tan_half_fov_x);
        const double pixels_per_meter =
            (safe_width * 0.5) * cam.screen_zoom / tan_half_fov_x;
        const double scale = cam.meters_scale * pixels_per_meter / depth_along_forward;
        if (!std::isfinite(scale) || scale <= 0.0) {
            return 1.0f;
        }
        float result = static_cast<float>(scale);
        if (std::isfinite(cam.near_camera_max_perspective_scale) &&
            cam.near_camera_max_perspective_scale > 0.0f) {
            result = std::min(result, cam.near_camera_max_perspective_scale);
        }
        return result;
    }

    ProjectionResult project_world_point_internal(const CameraState& cam,
                                         double world_x_pixels,
                                         double world_y_pixels,
                                         double world_z_pixels,
                                         int screen_width,
                                         int screen_height,
                                         float horizon_band_px) {
        ProjectionResult out{};
        if (!cam.valid) {
            return out;
        }

        const double meters_scale = cam.meters_scale;
        const double safe_scale = std::max(1e-6, meters_scale);
        const Vec3 world_meters{
            (world_x_pixels - static_cast<double>(cam.anchor_world_px.x)) * safe_scale,
            (world_y_pixels - static_cast<double>(cam.anchor_world_px.y)) * safe_scale,
            world_z_pixels * safe_scale
        };
        const Vec3 to_point_meters = world_meters - cam.position;
        const double depth_along_forward = dot(to_point_meters, cam.forward);
        const double distance_to_camera  = length(to_point_meters);
        if (depth_along_forward <= cam.near_plane ||
            distance_to_camera < cam.near_plane ||
            distance_to_camera > cam.far_plane ||
            !std::isfinite(distance_to_camera)) {
            return out;
        }

        const double cam_x = dot(to_point_meters, cam.right);
        const double cam_y = dot(to_point_meters, cam.up);

        const auto screen_opt = project_camera_space_point(
            cam, cam_x, cam_y, depth_along_forward, screen_width, screen_height);
        if (!screen_opt.has_value()) {
            return out;
        }
        const SDL_FPoint screen_pt = *screen_opt;
        const double screen_x = screen_pt.x;
        const double screen_y = screen_pt.y;

        float perspective_scale = compute_perspective_scale_from_depth(
            cam, depth_along_forward, screen_width);
        float vertical_scale = 1.0f;

        const double unit_meters = std::max(1e-6, cam.meters_scale);
        const Vec3 unit_world_x{ unit_meters, 0.0, 0.0 };
        const Vec3 unit_world_z{ 0.0, 0.0, unit_meters };
        const Vec3 delta_cam_x{
            dot(unit_world_x, cam.right),
            dot(unit_world_x, cam.up),
            dot(unit_world_x, cam.forward)
        };
        const Vec3 delta_cam_z{
            dot(unit_world_z, cam.right),
            dot(unit_world_z, cam.up),
            dot(unit_world_z, cam.forward)
        };
        auto screen_distance_for_delta = [&](const Vec3& delta_cam) -> std::optional<double> {
            const double depth2 = depth_along_forward + delta_cam.z;
            if (!std::isfinite(depth2) || depth2 <= cam.near_plane) {
                return std::nullopt;
            }
            const auto shifted = project_camera_space_point(
                cam,
                cam_x + delta_cam.x,
                cam_y + delta_cam.y,
                depth2,
                screen_width,
                screen_height);
            if (!shifted.has_value()) {
                return std::nullopt;
            }
            const double dx = shifted->x - screen_x;
            const double dy = shifted->y - screen_y;
            const double dist = std::sqrt(dx * dx + dy * dy);
            if (!std::isfinite(dist) || dist <= 1e-6) {
                return std::nullopt;
            }
            return dist;
        };

        if (const auto scale_x = screen_distance_for_delta(delta_cam_x)) {
            perspective_scale = static_cast<float>(*scale_x);
        }
        if (const auto scale_z = screen_distance_for_delta(delta_cam_z)) {
            if (std::isfinite(perspective_scale) && perspective_scale > 1e-6f) {
                vertical_scale = static_cast<float>(*scale_z / perspective_scale);
            }
        }
        if (std::isfinite(cam.near_camera_max_perspective_scale) &&
            cam.near_camera_max_perspective_scale > 0.0f) {
            perspective_scale = std::min(perspective_scale, cam.near_camera_max_perspective_scale);
        }
        if (!std::isfinite(perspective_scale) || perspective_scale <= 0.0f) {
            perspective_scale = 1.0f;
        }
        if (!std::isfinite(vertical_scale) || vertical_scale <= 0.0f) {
            vertical_scale = 1.0f;
        }

        const float zoom_scale = std::isfinite(cam.screen_zoom) && cam.screen_zoom > 0.0 ? static_cast<float>(cam.screen_zoom) : 1.0f;
        const float effective_horizon_band_px = horizon_band_px * zoom_scale;

        float horizon_fade = 1.0f;
        if (effective_horizon_band_px > 0.0f) {
            const float dist_from_horizon = static_cast<float>(screen_y - cam.horizon_screen_y);
            if (dist_from_horizon <= 0.0f) {
                horizon_fade = 0.0f;
            } else if (dist_from_horizon < effective_horizon_band_px) {
                const float t = dist_from_horizon / effective_horizon_band_px;
                horizon_fade = std::clamp(t * t * t, 0.0f, 1.0f);
            }
        }

        float near_camera_fade = 1.0f;
        if (std::isfinite(screen_y) && screen_height > 0) {
            const float screen_h = static_cast<float>(screen_height);
            const float offscreen_amount = std::max(0.0f, cam.offscreen_fade_amount_px);
            if (static_cast<float>(screen_y) >= screen_h) {
                if (offscreen_amount <= 0.0f) {
                    near_camera_fade = 0.0f;
                } else {
                    const float t = (static_cast<float>(screen_y) - screen_h) / offscreen_amount;
                    near_camera_fade = std::clamp(1.0f - t, 0.0f, 1.0f);
                }
            }
        }

        out.valid             = std::isfinite(screen_x) && std::isfinite(screen_y);
        out.screen            = SDL_FPoint{ static_cast<float>(screen_x), static_cast<float>(screen_y) };
        out.perspective_scale = perspective_scale;
        out.vertical_scale    = vertical_scale;
        out.horizon_fade      = horizon_fade;
        out.near_camera_fade  = near_camera_fade;
        out.distance          = std::isfinite(distance_to_camera) ? static_cast<float>(distance_to_camera) : 0.0f;
        out.forward_depth     = static_cast<float>(depth_along_forward);
        return out;
    }

    std::optional<SDL_FPoint> project_ndc_to_ground(const CameraState& cam, double ndc_x, double ndc_y) {
        if (!cam.valid) {
            return std::nullopt;
        }
        Vec3 dir = cam.forward;
        dir = dir + cam.right * (ndc_x * cam.tan_half_fov_x);
        dir = dir + cam.up * (ndc_y * cam.tan_half_fov_y);
        dir = normalize(dir);
        if (std::abs(dir.z) < 1e-6) {
            return std::nullopt;
        }
        const double t = -cam.position.z / dir.z;
        if (!std::isfinite(t) || t <= 0.0) {
            return std::nullopt;
        }
        const Vec3 hit = cam.position + dir * t;
        const double safe_scale = std::max(1e-6, cam.meters_scale);
        return SDL_FPoint{
            static_cast<float>(hit.x / safe_scale + static_cast<double>(cam.anchor_world_px.x)),
            static_cast<float>(hit.y / safe_scale + static_cast<double>(cam.anchor_world_px.y))
        };
    }

WarpedScreenGrid::FloorDepthParams build_floor_params_from_camera_state(
    int screen_width,
    int screen_height,
    const CameraState& cam) {
    WarpedScreenGrid::FloorDepthParams p{};
    if (!cam.valid) {
        return p;
    }

        const double screen_h = std::max(1.0, static_cast<double>(screen_height));
        constexpr double kMaxHorizonRatio = 0.45;
        const double max_horizon = screen_h * kMaxHorizonRatio;
        const double min_horizon = -screen_h * 4.0;

        const double horizon_y_raw = std::isfinite(cam.horizon_screen_y) ? cam.horizon_screen_y : screen_h * 0.5;
        const double horizon_y = std::clamp(horizon_y_raw, min_horizon, max_horizon);

        double pitch_norm = cam.pitch_radians / (kHalfFovY * 2.0);
        pitch_norm = std::clamp(pitch_norm, 0.0, 1.0);

        const auto horizon_ndc = screen_to_ndc_point(cam, 0.0, horizon_y, screen_width, screen_height).second;
        const auto bottom_ndc = screen_to_ndc_point(cam, 0.0, screen_h, screen_width, screen_height).second;

        p.horizon_screen_y = horizon_y;
        p.bottom_screen_y  = screen_h;
        p.base_world_y     = cam.anchor_world_y;
        p.camera_world_y   = cam.camera_world_y;
        p.camera_height    = cam.camera_height;
        p.pitch_radians    = cam.pitch_radians;
        p.pitch_norm       = pitch_norm;
        p.focus_depth      = cam.focus_depth;
        p.focus_ndc_offset = cam.focus_ndc_offset;
        p.horizon_ndc      = std::isfinite(horizon_ndc) ? horizon_ndc : 0.0;
        p.near_ndc         = std::isfinite(bottom_ndc) ? bottom_ndc : -1.0;
        p.ndc_scale        = 1.0;
        p.strength         = 6.0;
        p.enabled          = true;
        return p;
    }

    float warp_floor_screen_y_internal(
        float world_y,
        float linear_screen_y,
        const WarpedScreenGrid::FloorDepthParams& p,
        int screen_height) {
        if (!p.enabled ||
            !std::isfinite(p.horizon_screen_y) ||
            !std::isfinite(p.bottom_screen_y) ||
            !std::isfinite(p.camera_height) ||
            !std::isfinite(p.pitch_radians)) {
            return std::isfinite(linear_screen_y) ? linear_screen_y : 0.0f;
        }

        const double safe_linear_y = std::isfinite(linear_screen_y) ? static_cast<double>(linear_screen_y) : 0.0;

        const double horizon = p.horizon_screen_y;
        const double bottom  = p.bottom_screen_y;
        if (!std::isfinite(horizon) || !std::isfinite(bottom) || bottom <= horizon + 1e-3) {
            return static_cast<float>(safe_linear_y);
        }

        const double range = bottom - horizon;
        double t_linear = (safe_linear_y - horizon) / range;

        const double pitch = std::clamp(p.pitch_radians, 0.0, (PI_D / 4.0) * 2.0);
        const double base_strength = 0.5 + 0.5 * (pitch / ((PI_D / 4.0) * 2.0));
        const double strength = std::clamp(p.strength, 0.0, 10.0);
        const double k = 1.0 + strength * base_strength;

        double t_warp;
        if (t_linear >= 0.0) {
            t_warp = std::pow(t_linear, k);
        } else {
            const double pitch_factor = std::clamp(pitch / ((PI_D / 4.0) * 2.0), 0.1, 1.0);
            const double decay_rate = 0.03 + 0.15 * pitch_factor;
            const double exp_decay = std::exp(t_linear * decay_rate);
            const double scale_factor = 0.005 + 0.015 * pitch_factor;
            t_warp = -exp_decay * scale_factor;
            t_warp = std::min(t_warp, -0.00001);
        }

        double screen_y = horizon + t_warp * range;
        screen_y = std::max(screen_y, horizon);
        if (!std::isfinite(screen_y)) {
            return static_cast<float>(safe_linear_y);
        }

        (void)world_y;
        (void)screen_height;
        return static_cast<float>(screen_y);
    }

    double compute_floor_distance_measure(double screen_y, const WarpedScreenGrid::FloorDepthParams& params) {
        if (!params.enabled) {
            return 0.0;
        }

        const double min_bound = std::min(params.horizon_screen_y, params.bottom_screen_y);
        const double max_bound = std::max(params.horizon_screen_y, params.bottom_screen_y);
        const double clamped_y = std::clamp(static_cast<double>(screen_y), min_bound, max_bound);

        const double denom_screen = std::max(1e-4, std::abs(params.bottom_screen_y - params.horizon_screen_y));
        const double t_screen = std::clamp((clamped_y - params.horizon_screen_y) / denom_screen, 0.0, 1.0);
        const double ndc_y = params.horizon_ndc + (params.near_ndc - params.horizon_ndc) * t_screen;
        const double ndc_span = std::max(1e-4, std::abs(params.near_ndc - params.horizon_ndc));
        return (params.near_ndc - ndc_y) / ndc_span;
    }

}

WarpedScreenGrid::WarpedScreenGrid(int screen_width, int screen_height, const Area& starting_view)
{
    screen_width_  = screen_width;
    screen_height_ = screen_height;
    aspect_        = (screen_height_ > 0) ? static_cast<double>(screen_width_) / static_cast<double>(screen_height_) : 1.0;

    Area      adjusted_start = convert_area_to_aspect(starting_view);
    SDL_Point start_center   = adjusted_start.get_center();

    base_view_    = make_rect_area("base_view", start_center, screen_width_, screen_height_, adjusted_start.resolution());
    current_view_ = adjusted_start;

    CameraParams initial_params;
    initial_params.height_px = settings_.base_height_px;
    initial_params.tilt_deg = camera_math::kDefaultCameraTiltDeg;
    initial_params.y_distance_px = 0.0;
    camera_.reset(initial_params, start_center, settings_.base_height_px);
    runtime_camera_height_ = camera_.current_height();
    runtime_pitch_deg_ = camera_.current_pitch_deg();
    runtime_pitch_rad_ = camera_.current_pitch_rad();
    runtime_anchor_world_y_ = camera_.state().center.y;
    depth_enabled_   = true;
}

WarpedScreenGrid::~WarpedScreenGrid() = default;

void WarpedScreenGrid::set_frustum_padding_world(float padding) {
    frustum_padding_world_ = std::max(0.0f, padding);
}

void WarpedScreenGrid::set_realism_settings(const RealismSettings& settings) {
    settings_ = settings;
    if (!std::isfinite(settings_.base_height_px) || settings_.base_height_px <= 0.0f) {
        settings_.base_height_px = 720.0f;
    }
    camera_.set_fallback_height(settings_.base_height_px);
    settings_.near_camera_max_perspective_scale =
        std::clamp(settings_.near_camera_max_perspective_scale, 0.0f, 100.0f);
    settings_.offscreen_fade_amount_px =
        std::clamp(settings_.offscreen_fade_amount_px, 0.0f, 1000.0f);

    // No-op: geometry cache is obsolete.
}

void WarpedScreenGrid::set_screen_center(SDL_Point p, bool snap_immediately) {
    camera_.set_screen_center(p, snap_immediately);
}



double WarpedScreenGrid::compute_room_scale_from_area(const Room* room) const {
    if (!room) {
        return camera_.current_height();
    }
    return std::max(1.0, static_cast<double>(room->camera_height_px));
}

void WarpedScreenGrid::set_up_rooms(CurrentRoomFinder* finder) {
    if (!finder) return;
    Room* current = finder->getCurrentRoom();
    if (!current) return;

    starting_room_ = current;
    if (starting_room_ && starting_room_->room_area) {
        Area adjusted = convert_area_to_aspect(*starting_room_->room_area);
        starting_area_ = adjusted.get_size();
        if (starting_area_ <= 0.0) starting_area_ = 1.0;
    }
}

// --- Refactored: Use explicit per-room camera parameters and interpolate between them ---


void WarpedScreenGrid::animate_height_to_scale(double target_height_px, int steps) {
    camera_.animate_height_to(target_height_px, steps);
}

void WarpedScreenGrid::update_camera_height(Room* cur,
                         CurrentRoomFinder* finder,
                         Asset* player,
                         bool refresh_requested,
                         float dt,
                         bool dev_mode)
{
    CameraParams cur_params;
    CameraParams neigh_params;
    double t = 0.0;
    const double fallback_height = std::max(1.0, static_cast<double>(settings_.base_height_px));

    if (cur) {
        cur_params.height_px = cur->camera_height_px;
        cur_params.tilt_deg = cur->camera_tilt_deg;
        cur_params.y_distance_px = cur->camera_y_distance_px;
        cur_params.zoom_percent = cur->camera_zoom_percent;
  
    }
    Room* neigh = nullptr;
    if (finder) {
        neigh = finder->getNeighboringRoom(cur);
    }
    if (!neigh) neigh = cur;
    if (neigh) {
        neigh_params.height_px = neigh->camera_height_px;
        neigh_params.tilt_deg = neigh->camera_tilt_deg;
        neigh_params.y_distance_px = neigh->camera_y_distance_px;
        neigh_params.zoom_percent = neigh->camera_zoom_percent;

    }
    if (tilt_override_deg_) {
        cur_params.tilt_deg = *tilt_override_deg_;
        neigh_params.tilt_deg = *tilt_override_deg_;
    }
    cur_params = camera_math::sanitize_camera_params(cur_params, fallback_height);
    neigh_params = camera_math::sanitize_camera_params(neigh_params, fallback_height);

    // Interpolation factor t: if player is between rooms, interpolate; else use cur
    if (player && cur && cur->room_area && neigh && neigh->room_area && cur != neigh) {
        auto [ax, ay] = cur->room_area->get_center();
        auto [bx, by] = neigh->room_area->get_center();
        const double pax = double(player->pos.x);
        const double pay = double(player->pos.y);
        const double vx = double(bx - ax);
        const double vy = double(by - ay);
        const double wx = double(pax - ax);
        const double wy = double(pay - ay);
        const double vlen2 = vx * vx + vy * vy;
        t = (vlen2 > 0.0) ? ((wx * vx + wy * vy) / vlen2) : 0.0;
        t = std::clamp(t, 0.0, 1.0);
    }

    camera_.apply_room_targets(cur_params, neigh_params, t, refresh_requested, 20, dev_mode);

    if (camera_.has_focus_override()) {
        set_screen_center(camera_.state().focus_override);
    } else if (player) {
        set_screen_center(SDL_Point{ player->pos.x, player->pos.y }, false);
    } else if (cur && cur->room_area) {
        set_screen_center(cur->room_area->get_center());
    }

    camera_.tick(dt);
    runtime_camera_height_ = camera_.current_height();
    runtime_pitch_deg_ = camera_.current_pitch_deg();
    runtime_pitch_rad_ = camera_.current_pitch_rad();
    runtime_anchor_world_y_ = camera_.state().center.y;
}

Area WarpedScreenGrid::convert_area_to_aspect(const Area& in) const {
    auto [minx, miny, maxx, maxy] = in.get_bounds();
    int w = std::max(1, maxx - minx);
    int h = std::max(1, maxy - miny);
    SDL_Point c = in.get_center();

    const double cur = static_cast<double>(w) / static_cast<double>(h);
    int target_w = w;
    int target_h = h;
    if (cur < aspect_) {
        target_w = static_cast<int>(std::lround(static_cast<double>(h) * aspect_));
    } else if (cur > aspect_) {
        target_h = static_cast<int>(std::lround(static_cast<double>(w) / aspect_));
    }
    return make_rect_area("adjusted_" + in.get_name(), c, target_w, target_h, in.resolution());
}

void WarpedScreenGrid::recompute_current_view() {
    const CameraController::State& cam_settings = camera_.state();
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, cam_settings.center, cam_settings.params);
    runtime_camera_height_ = cam.camera_height;
    runtime_focus_depth_ = cam.focus_depth;
    runtime_anchor_world_y_ = cam_settings.center.y;
    runtime_focus_ndc_offset_ = cam.focus_ndc_offset;
    runtime_pitch_rad_ = cam.pitch_radians;
    runtime_pitch_deg_ = cam.pitch_degrees;
    runtime_depth_offset_px_ = static_cast<float>(cam.reference_depth);

    std::vector<SDL_FPoint> ground_points;
    ground_points.reserve(4);
    const ScreenBounds virtual_bounds = expanded_screen_bounds(screen_width_, screen_height_);
    const std::array<SDL_FPoint, 4> screen_corners{
        SDL_FPoint{virtual_bounds.left, virtual_bounds.top},
        SDL_FPoint{virtual_bounds.right, virtual_bounds.top},
        SDL_FPoint{virtual_bounds.right, virtual_bounds.bottom},
        SDL_FPoint{virtual_bounds.left, virtual_bounds.bottom}
    };
    for (const auto& corner : screen_corners) {
        const auto [nx, ny] = screen_to_ndc_point(
            cam,
            static_cast<double>(corner.x),
            static_cast<double>(corner.y),
            screen_width_,
            screen_height_);
        auto gp = project_ndc_to_ground(cam, nx, ny);
        if (gp.has_value()) {
            ground_points.push_back(*gp);
        }
    }

    if (ground_points.empty()) {
        SDL_Point center{
            static_cast<int>(std::lround(cam_settings.center.x)), static_cast<int>(std::lround(cam_settings.center.y)) };
        const int virtual_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(screen_width_) * kVirtualScreenScale)));
        const int virtual_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(screen_height_) * kVirtualScreenScale)));
        current_view_ = make_rect_area("current_view", center, virtual_w, virtual_h, 0);
        return;
    }

    float minx = ground_points.front().x;
    float maxx = ground_points.front().x;
    float miny = ground_points.front().y;
    float maxy = ground_points.front().y;
    for (const auto& p : ground_points) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }

    const float margin = std::max(0.0f, settings_.extra_cull_margin + frustum_padding_world_);
    minx -= margin;
    maxx += margin;
    miny -= margin;
    maxy += margin;

    const int cur_w = std::max(1, static_cast<int>(std::lround(maxx - minx)));
    const int cur_h = std::max(1, static_cast<int>(std::lround(maxy - miny)));
    SDL_Point center{
        static_cast<int>(std::lround((minx + maxx) * 0.5f)),
        static_cast<int>(std::lround((miny + maxy) * 0.5f))
    };
    current_view_ = make_rect_area("current_view", center, cur_w, cur_h, 0);
}



SDL_FPoint WarpedScreenGrid::map_to_screen(SDL_Point world) const {
    SDL_FPoint world_f{ static_cast<float>(world.x), static_cast<float>(world.y) };
    return map_to_screen_f(world_f);
}

SDL_FPoint WarpedScreenGrid::map_to_screen_f(SDL_FPoint world) const {
    const CameraController::State& cam_settings = camera_.state();
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, cam_settings.center, cam_settings.params);
    ProjectionResult proj = project_world_point_internal(cam,
                                                static_cast<double>(world.x),
                                                static_cast<double>(world.y),
                                                0.0,
                                                screen_width_,
                                                screen_height_,
                                                horizon_fade_for_height(cam.camera_height));
    if (!proj.valid) {
        return SDL_FPoint{0.0f, 0.0f};
    }
    return proj.screen;
}

bool WarpedScreenGrid::project_world_point(SDL_FPoint world, float world_z, SDL_FPoint& out) const {
    const CameraController::State& cam_settings = camera_.state();
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, cam_settings.center, cam_settings.params);
    ProjectionResult proj = project_world_point_internal(cam,
                                                static_cast<double>(world.x),
                                                static_cast<double>(world.y),
                                                static_cast<double>(world_z),
                                                screen_width_,
                                                screen_height_,
                                                horizon_fade_for_height(cam.camera_height));
    if (!proj.valid) {
        return false;
    }
    out = proj.screen;
    return true;
}

SDL_FPoint WarpedScreenGrid::screen_to_map(SDL_Point screen) const {
    const CameraController::State& cam_settings = camera_.state();
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, cam_settings.center, cam_settings.params);
    if (!cam.valid) {
        return SDL_FPoint{0.0f, 0.0f};
    }

    const auto [ndc_x, ndc_y] = screen_to_ndc_point(
        cam,
        static_cast<double>(screen.x),
        static_cast<double>(screen.y),
        screen_width_,
        screen_height_);

    auto ground_point = project_ndc_to_ground(cam, ndc_x, ndc_y);
    if (!ground_point.has_value()) {
        return SDL_FPoint{0.0f, 0.0f};
    }
    return *ground_point;
}
WarpedScreenGrid::RenderEffects WarpedScreenGrid::compute_render_effects(
    SDL_Point world,
    float ,
    float ,
    RenderSmoothingKey ,
    int world_z) const
{
    RenderEffects result;

    const CameraController::State& cam_settings = camera_.state();
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, cam_settings.center, cam_settings.params);
    ProjectionResult proj = project_world_point_internal(cam,
                                                static_cast<double>(world.x),
                                                static_cast<double>(world.y),
                                                static_cast<double>(world_z),
                                                screen_width_,
                                                screen_height_,
                                                horizon_fade_for_height(cam.camera_height));
    if (!proj.valid) {
        result.vertical_scale     = 0.0f;
        result.distance_scale     = 0.0f;
        result.horizon_fade_alpha = 0.0f;
        return result;
    }

    result.screen_position   = proj.screen;
    result.vertical_scale    = proj.vertical_scale;
    result.distance_scale    = proj.perspective_scale;
    result.horizon_fade_alpha = proj.horizon_fade * proj.near_camera_fade;
    return result;
}

void WarpedScreenGrid::apply_camera_settings(const nlohmann::json& data) {
    if (!data.is_object()) {
        return;
    }

    auto read_float = [&](const char* key, float& target, float min_value, float max_value) {
        auto it = data.find(key);
        if (it == data.end() || !it->is_number()) {
            return;
        }
        const float value = it->get<float>();
        if (!std::isfinite(value)) {
            return;
        }
        target = std::clamp(value, min_value, max_value);
    };
    auto read_int = [&](const char* key, int& target, int min_value, int max_value) {
        auto it = data.find(key);
        if (it == data.end() || !it->is_number_integer()) {
            return;
        }
        const int value = it->get<int>();
        target = std::clamp(value, min_value, max_value);
    };
    auto read_effect_settings = [&](const char* key, camera_effects::ImageEffectSettings& target) {
        auto it = data.find(key);
        if (it == data.end() || !it->is_object()) {
            return;
        }
        const nlohmann::json& obj = *it;
        if (obj.contains("contrast") && obj["contrast"].is_number()) {
            target.contrast = obj["contrast"].get<float>();
        }
        if (obj.contains("brightness") && obj["brightness"].is_number()) {
            target.brightness = obj["brightness"].get<float>();
        }
        if (obj.contains("blur") && obj["blur"].is_number()) {
            target.blur = obj["blur"].get<float>();
        }
        if (obj.contains("saturation_red") && obj["saturation_red"].is_number()) {
            target.saturation_red = obj["saturation_red"].get<float>();
        }
        if (obj.contains("saturation_green") && obj["saturation_green"].is_number()) {
            target.saturation_green = obj["saturation_green"].get<float>();
        }
        if (obj.contains("saturation_blue") && obj["saturation_blue"].is_number()) {
            target.saturation_blue = obj["saturation_blue"].get<float>();
        }
        if (obj.contains("hue") && obj["hue"].is_number()) {
            target.hue = obj["hue"].get<float>();
        }
        camera_effects::ClampImageEffectSettings(target);
    };

    RealismSettings updated = settings_;
    read_float("min_visible_screen_ratio", updated.min_visible_screen_ratio, 0.0f, 0.5f);
    read_float("base_height_px", updated.base_height_px, 1.0f, 100000.0f);
    read_int("render_quality_percent", updated.render_quality_percent, 10, 100);
    read_float("meters_per_100_world_px", updated.meters_per_100_world_px, 0.01f, 1000.0f);

    read_float("extra_cull_margin", updated.extra_cull_margin, 0.0f, 10000.0f);
    read_float("near_camera_max_perspective_scale", updated.near_camera_max_perspective_scale, 0.0f, 100.0f);
    read_float("offscreen_fade_amount_px", updated.offscreen_fade_amount_px, 0.0f, 1000.0f);


    set_realism_settings(updated);
}

nlohmann::json WarpedScreenGrid::camera_settings_to_json() const {
    nlohmann::json result = nlohmann::json::object();
    result["min_visible_screen_ratio"] = settings_.min_visible_screen_ratio;
    result["base_height_px"] = settings_.base_height_px;
    result["render_quality_percent"] = settings_.render_quality_percent;
    result["meters_per_100_world_px"] = settings_.meters_per_100_world_px;
    result["extra_cull_margin"] = settings_.extra_cull_margin;
    result["near_camera_max_perspective_scale"] = settings_.near_camera_max_perspective_scale;
    result["offscreen_fade_amount_px"] = settings_.offscreen_fade_amount_px;


    return result;
}
SDL_FPoint WarpedScreenGrid::get_view_center_f() const {
    const SDL_FPoint center = camera_.state().center;
    if (std::isfinite(center.x) && std::isfinite(center.y)) {
        return center;
    }
    int left, top, right, bottom;
    std::tie(left, top, right, bottom) = current_view_.get_bounds();
    const float cx = (static_cast<float>(left) + static_cast<float>(right)) * 0.5f;
    const float cy = (static_cast<float>(top)  + static_cast<float>(bottom)) * 0.5f;
    return SDL_FPoint{ cx, cy };
}

WarpedScreenGrid::FloorDepthParams WarpedScreenGrid::compute_floor_depth_params() const {
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, camera_.state().center, camera_.state().params);
    return build_floor_params_from_camera_state(screen_width_, screen_height_, cam);
}

float WarpedScreenGrid::warp_floor_screen_y(float world_y, float linear_screen_y) const {
    (void)world_y;

    return std::isfinite(linear_screen_y) ? linear_screen_y : 0.0f;
}

double WarpedScreenGrid::view_height_world() const {
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    std::tie(minx, miny, maxx, maxy) = current_view_.get_bounds();
    return static_cast<double>(std::max(0, maxy - miny));
}

double WarpedScreenGrid::anchor_world_y() const {

    return static_cast<double>(camera_.state().center.y);
}

void WarpedScreenGrid::clear_grid_state() {
    warped_points_.clear();
    visible_assets_.clear();
    visible_points_.clear();
    active_chunks_.clear();
    asset_to_point_.clear();
    cached_world_rect_ = SDL_Rect{0, 0, 0, 0};
    bounds_ = GridBounds{};
}

void WarpedScreenGrid::rebuild_grid_bounds() {
    if (warped_points_.empty()) {
        cached_world_rect_ = SDL_Rect{0, 0, 0, 0};
        bounds_ = GridBounds{};
        return;
    }

    int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
    for (const world::GridPoint* gp : warped_points_) {
        if (!gp) continue;
        minx = std::min(minx, gp->world.x);
        miny = std::min(miny, gp->world.y);
        maxx = std::max(maxx, gp->world.x);
        maxy = std::max(maxy, gp->world.y);
    }
    if (minx > maxx || miny > maxy) {
        cached_world_rect_ = SDL_Rect{0, 0, 0, 0};
        bounds_ = GridBounds{};
        return;
    }
    cached_world_rect_.x = minx;
    cached_world_rect_.y = miny;
    cached_world_rect_.w = std::max(0, maxx - minx);
    cached_world_rect_.h = std::max(0, maxy - miny);

    bounds_.left = 0.0f;
    bounds_.top = 0.0f;
    bounds_.right = static_cast<float>(screen_width_);
    bounds_.bottom = static_cast<float>(screen_height_);
}

void WarpedScreenGrid::rebuild_grid(world::WorldGrid& world_grid, float dt_seconds) {
    (void)dt_seconds;
    const std::uint64_t frame_stamp = ++frame_counter_;
    clear_grid_state();

    // Refresh the world-space view area based on the latest camera parameters.
    recompute_current_view();

    int minx, miny, maxx, maxy;
    std::tie(minx, miny, maxx, maxy) = current_view_.get_bounds();
    SDL_FRect world_bounds{
        static_cast<float>(minx),
        static_cast<float>(miny),
        static_cast<float>(std::max(0, maxx - minx)),
        static_cast<float>(std::max(0, maxy - miny))
};
    world::WorldGrid::RegionMetrics region_metrics{};
    const int min_world_z = static_cast<int>(std::floor(settings_.depth_near_world));
    const int max_world_z = static_cast<int>(std::ceil(settings_.depth_far_world));
    std::vector<world::GridPoint*> grid_points = world_grid.query_region(
        world_bounds,
        0,
        world_grid.max_resolution_layers(),
        min_world_z,
        max_world_z,
        /*skip_inactive_branches=*/true,
        /*include_empty_nodes=*/false,
        &region_metrics);
    last_nodes_visited_ = region_metrics.nodes_visited;
    last_branches_skipped_ = region_metrics.branches_skipped;
    warped_points_.reserve(grid_points.size());
    visible_points_.reserve(grid_points.size());
    visible_assets_.reserve(grid_points.size());

    const float screen_w    = static_cast<float>(screen_width_);
    const float screen_h    = static_cast<float>(screen_height_);
    const ScreenBounds virtual_bounds = expanded_screen_bounds(screen_width_, screen_height_);
    const float virtual_w = virtual_bounds.right - virtual_bounds.left;
    const float virtual_h = virtual_bounds.bottom - virtual_bounds.top;

    const CameraController::State& cam_settings = camera_.state();
    const CameraState cam_state = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, cam_settings.center, cam_settings.params);
    runtime_camera_height_ = cam_state.camera_height;
    runtime_focus_depth_ = cam_state.focus_depth;
    runtime_anchor_world_y_ = cam_settings.center.y;
    runtime_focus_ndc_offset_ = cam_state.focus_ndc_offset;
    runtime_pitch_rad_ = cam_state.pitch_radians;
    runtime_pitch_deg_ = cam_state.pitch_degrees;
    runtime_depth_offset_px_ = static_cast<float>(cam_state.reference_depth);
    if (!cam_state.valid) {
        last_min_world_z_ = 0;
        last_max_world_z_ = 0;
        last_depth_culled_ = 0;
        rebuild_grid_bounds();
        return;
    }
    const float horizon_screen_y = static_cast<float>(cam_state.horizon_screen_y);
    const float horizon_band = horizon_fade_for_height(cam_state.camera_height);

    const float world_margin_units = std::max(0.0f, settings_.extra_cull_margin);
    const float meters_per_100 = std::max(0.0001f, settings_.meters_per_100_world_px);
    const float pixels_per_world_unit = 100.0f / meters_per_100;
    const float margin_px = world_margin_units * pixels_per_world_unit;
    const float cull_top = std::clamp(static_cast<float>(cam_state.horizon_screen_y) - margin_px, virtual_bounds.top, virtual_bounds.bottom);
    const SDL_FRect cull_rect{
        virtual_bounds.left - margin_px,
        cull_top,
        virtual_w + margin_px * 2.0f,
        virtual_bounds.bottom - cull_top + margin_px
};
    const float min_visible_px =
        screen_h * std::clamp(settings_.min_visible_screen_ratio, 0.0f, 0.5f);
    last_min_world_z_ = std::numeric_limits<int>::max();
    last_max_world_z_ = std::numeric_limits<int>::min();
    last_depth_culled_ = 0;

    const double padding_world = std::max(0.0f, frustum_padding_world_);
    const double safe_scale = std::max(1e-6, cam_state.meters_scale);
    const double padding_meters = padding_world * safe_scale;
    const double padded_near = std::max(0.0, cam_state.near_plane - padding_meters);
    const double padded_far  = cam_state.far_plane + padding_meters;

    auto project_screen_point = [&](double world_x, double world_y, double world_z, SDL_FPoint& out) -> bool {
        ProjectionResult projected = project_world_point_internal(cam_state,
                                                         world_x,
                                                         world_y,
                                                         world_z,
                                                         screen_width_,
                                                         screen_height_,
                                                         horizon_band);
        if (!projected.valid) {
            return false;
        }
        if (!std::isfinite(projected.screen.x) || !std::isfinite(projected.screen.y)) {
            return false;
        }
        out = projected.screen;
        return true;
    };

    struct CameraSpaceData {
        double depth = 0.0;
        double distance = 0.0;
        double cam_x = 0.0;
        double cam_y = 0.0;
        bool valid = false;
    };

    auto to_camera_space = [&](double world_x, double world_y, double world_z) -> CameraSpaceData {
        CameraSpaceData data{};
        const Vec3 world_meters{
            (world_x - static_cast<double>(cam_state.anchor_world_px.x)) * safe_scale,
            (world_y - static_cast<double>(cam_state.anchor_world_px.y)) * safe_scale,
            world_z * safe_scale
        };
        const Vec3 to_point = world_meters - cam_state.position;
        data.depth = dot(to_point, cam_state.forward);
        data.distance = length(to_point);
        data.cam_x = dot(to_point, cam_state.right);
        data.cam_y = dot(to_point, cam_state.up);
        data.valid = std::isfinite(data.depth) && std::isfinite(data.distance);
        return data;
    };

    auto point_inside_frustum = [&](double world_x, double world_y, double world_z) -> bool {
        const CameraSpaceData cs = to_camera_space(world_x, world_y, world_z);
        if (!cs.valid) {
            return false;
        }
        if (cs.distance < padded_near || cs.depth < padded_near || cs.distance > padded_far) {
            return false;
        }
        const double half_w = cs.depth * cam_state.tan_half_fov_x * kVirtualScreenScale + padding_meters;
        const double half_h = cs.depth * cam_state.tan_half_fov_y * kVirtualScreenScale + padding_meters;
        return std::abs(cs.cam_x) <= half_w && std::abs(cs.cam_y) <= half_h;
    };

    struct Bounds2D {
        double left = 0.0;
        double right = 0.0;
        double top = 0.0;
        double bottom = 0.0;
    };

    auto compute_bounds = [&](Asset* asset) -> std::optional<Bounds2D> {
        if (!asset || !asset->info) {
            return std::nullopt;
        }
        if (const auto& tiling = asset->tiling_info(); tiling && tiling->is_valid()) {
            return Bounds2D{
                static_cast<double>(tiling->coverage.x),
                static_cast<double>(tiling->coverage.x + tiling->coverage.w),
                static_cast<double>(tiling->coverage.y),
                static_cast<double>(tiling->coverage.y + tiling->coverage.h)
            };
        }

        float scale = asset->smoothed_scale();
        if (!std::isfinite(scale) || scale <= 0.0f) {
            if (asset->info && std::isfinite(asset->info->scale_factor) && asset->info->scale_factor > 0.0f) {
                scale = asset->info->scale_factor;
            } else {
                scale = 1.0f;
            }
        }

        const float fw = static_cast<float>(std::max(1, asset->info->original_canvas_width));
        const float fh = static_cast<float>(std::max(1, asset->info->original_canvas_height));
        const float width = fw * scale;
        const float height = fh * scale;
        const float half_w = width * 0.5f;

        float center_x = asset->smoothed_translation_x();
        float bottom = asset->smoothed_translation_y();
        if (!std::isfinite(center_x)) {
            center_x = static_cast<float>(asset->pos.x);
        }
        if (!std::isfinite(bottom)) {
            bottom = static_cast<float>(asset->pos.y);
        }

        return Bounds2D{
            static_cast<double>(center_x - half_w),
            static_cast<double>(center_x + half_w),
            static_cast<double>(bottom - height),
            static_cast<double>(bottom)
        };
    };

    auto asset_in_frustum = [&](Asset* asset, const world::GridPoint* gp) -> bool {
        if (!asset) {
            return false;
        }
        const double asset_z = gp ? static_cast<double>(gp->world_z()) : 0.0;
        if (auto bounds = compute_bounds(asset)) {
            const Bounds2D& b = *bounds;
            const std::array<std::pair<double, double>, 4> corners{
                std::pair<double, double>{b.left, b.bottom},
                std::pair<double, double>{b.right, b.bottom},
                std::pair<double, double>{b.left, b.top},
                std::pair<double, double>{b.right, b.top}
            };
            for (const auto& [cx, cy] : corners) {
                if (point_inside_frustum(cx, cy, asset_z)) {
                    return true;
                }
            }
        }
        float center_x = asset->smoothed_translation_x();
        float center_y = asset->smoothed_translation_y();
        if (!std::isfinite(center_x)) {
            center_x = static_cast<float>(asset->pos.x);
        }
        if (!std::isfinite(center_y)) {
            center_y = static_cast<float>(asset->pos.y);
        }
        return point_inside_frustum(static_cast<double>(center_x), static_cast<double>(center_y), asset_z);
    };

    std::vector<Asset*> frustum_hits;
    frustum_hits.reserve(8);

    for (world::GridPoint* gp : grid_points) {
        if (!gp) continue;
        const SDL_FPoint prev_screen = gp->screen;
        const bool prev_screen_valid = gp->screen_data_valid &&
                                       std::isfinite(prev_screen.x) &&
                                       std::isfinite(prev_screen.y);
        gp->reset_frame_state(frame_stamp);

        if (gp->occupants.empty()) {
            continue;
        }

        Asset* primary_asset = nullptr;
        for (const auto& owned : gp->occupants) {
            if (owned) {
                primary_asset = owned.get();
                break;
            }
        }
        if (!primary_asset) {
            continue;
        }

        const SDL_Point world_pos{ gp->world_x(), gp->world_y() };
        const CameraSpaceData space = to_camera_space(
            static_cast<double>(world_pos.x),
            static_cast<double>(world_pos.y),
            static_cast<double>(gp->world_z()));
        ProjectionResult proj = project_world_point_internal(
            cam_state,
            static_cast<double>(world_pos.x),
            static_cast<double>(world_pos.y),
            static_cast<double>(gp->world_z()),
            screen_width_,
            screen_height_,
            horizon_band);
        if (!proj.valid) {
            proj.screen = prev_screen_valid
                ? prev_screen
                : SDL_FPoint{ static_cast<float>(world_pos.x), static_cast<float>(world_pos.y) };
            proj.perspective_scale = 1.0f;
            proj.vertical_scale = 1.0f;
            proj.horizon_fade = 1.0f;
            proj.distance = std::isfinite(space.distance) ? static_cast<float>(space.distance) : 0.0f;
        }

        SDL_FPoint screen_pos = proj.screen;
        if (!std::isfinite(screen_pos.x) || !std::isfinite(screen_pos.y)) {
            screen_pos = prev_screen_valid ? prev_screen : SDL_FPoint{0.0f, 0.0f};
        }

        const SDL_FPoint screen_for_bounds = screen_pos;

        float base_scale = primary_asset->smoothed_scale();
        if (!std::isfinite(base_scale) || base_scale <= 0.0f) {
            base_scale = 1.0f;
        }

        const int fw = (primary_asset && primary_asset->info) ? std::max(1, primary_asset->info->original_canvas_width) : 1;
        const int fh = (primary_asset && primary_asset->info) ? std::max(1, primary_asset->info->original_canvas_height) : 1;

        const float world_width = static_cast<float>(fw) * base_scale;
        const float world_height = static_cast<float>(fh) * base_scale;
        const float half_width = 0.5f * world_width;

        bool have_projected_bounds = false;
        float min_x = 0.0f;
        float max_x = 0.0f;
        float min_y = 0.0f;
        float max_y = 0.0f;
        auto expand_bounds = [&](const SDL_FPoint& pt) {
            if (!have_projected_bounds) {
                min_x = max_x = pt.x;
                min_y = max_y = pt.y;
                have_projected_bounds = true;
                return;
            }
            min_x = std::min(min_x, pt.x);
            max_x = std::max(max_x, pt.x);
            min_y = std::min(min_y, pt.y);
            max_y = std::max(max_y, pt.y);
        };

        if (world_width > 0.0f && world_height > 0.0f) {
            SDL_FPoint corner{};
            if (project_screen_point(world_pos.x - half_width, world_pos.y, 0.0, corner)) {
                expand_bounds(corner);
            }
            if (project_screen_point(world_pos.x + half_width, world_pos.y, 0.0, corner)) {
                expand_bounds(corner);
            }
            if (project_screen_point(world_pos.x - half_width, world_pos.y, world_height, corner)) {
                expand_bounds(corner);
            }
            if (project_screen_point(world_pos.x + half_width, world_pos.y, world_height, corner)) {
                expand_bounds(corner);
            }
        }

        const float min_size = std::max(1.0f, min_visible_px);
        SDL_FRect bounds{};
        if (have_projected_bounds) {
            float width = max_x - min_x;
            float height = max_y - min_y;
            if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f) {
                have_projected_bounds = false;
            } else {
                width = std::max(width, min_size);
                height = std::max(height, min_size);
                const float center_x = (min_x + max_x) * 0.5f;
                const float center_y = (min_y + max_y) * 0.5f;
                bounds = SDL_FRect{
                    center_x - width * 0.5f,
                    center_y - height * 0.5f,
                    width,
                    height
                };
            }
        }
        if (!have_projected_bounds) {
            const float size = min_size;
            bounds = SDL_FRect{
                screen_for_bounds.x - size * 0.5f,
                screen_for_bounds.y - size,
                size,
                size
            };
        }

        const float distance_to_cam = std::isfinite(proj.distance)
            ? proj.distance
            : static_cast<float>(space.distance);

        last_min_world_z_ = std::min(last_min_world_z_, gp->world_z());
        last_max_world_z_ = std::max(last_max_world_z_, gp->world_z());
        if (space.valid && (space.distance < padded_near || space.distance > padded_far)) {
            ++last_depth_culled_;
        }

        frustum_hits.clear();
        for (const auto& owned : gp->occupants) {
            if (owned) {
                asset_to_point_[owned.get()] = gp;
                if (asset_in_frustum(owned.get(), gp)) {
                    frustum_hits.push_back(owned.get());
                }
            }
        }

        gp->screen             = screen_for_bounds;
        gp->parallax_dx        = 0.0f;
        gp->vertical_scale     = proj.vertical_scale;
        gp->horizon_fade_alpha = proj.horizon_fade;
        gp->near_camera_fade_alpha = proj.near_camera_fade;

        gp->perspective_scale  = proj.perspective_scale;
        gp->distance_to_camera = distance_to_cam;
        gp->tilt_radians       = static_cast<float>(runtime_pitch_rad_);
        gp->on_screen          = !frustum_hits.empty();
        gp->mark_screen_data_updated(frame_stamp);

        warped_points_.push_back(gp);
        if (!frustum_hits.empty()) {
            visible_points_.push_back(gp);
            visible_assets_.insert(visible_assets_.end(), frustum_hits.begin(), frustum_hits.end());
        }
        if (gp->chunk) active_chunks_.push_back(gp->chunk);
    }

    if (!active_chunks_.empty()) {
        std::sort(active_chunks_.begin(), active_chunks_.end());
        active_chunks_.erase(std::unique(active_chunks_.begin(), active_chunks_.end()), active_chunks_.end());
    }

    if (last_min_world_z_ == std::numeric_limits<int>::max()) {
        last_min_world_z_ = 0;
        last_max_world_z_ = 0;
    }
    if (depth_debug_logging_) {
        vibble::log::debug("[WarpedScreenGrid] frame=" + std::to_string(frame_stamp) +
                           " nodes=" + std::to_string(last_nodes_visited_) +
                           " branches_skipped=" + std::to_string(last_branches_skipped_) +
                           " depth_culled=" + std::to_string(last_depth_culled_) +
                           " z_range=[" + std::to_string(last_min_world_z_) + "," + std::to_string(last_max_world_z_) + "]");
    }

    rebuild_grid_bounds();
    bounds_.left   = cull_rect.x;
    bounds_.top    = cull_rect.y;
    bounds_.right  = cull_rect.x + cull_rect.w;
    bounds_.bottom = cull_rect.y + cull_rect.h;
}

world::GridPoint* WarpedScreenGrid::grid_point_for_asset(const Asset* asset) {
    if (!asset) return nullptr;
    auto it = asset_to_point_.find(asset);
    if (it != asset_to_point_.end()) {
        return it->second;
    }
    return nullptr;
}

const world::GridPoint* WarpedScreenGrid::grid_point_for_asset(const Asset* asset) const {
    if (!asset) return nullptr;
    auto it = asset_to_point_.find(asset);
    if (it != asset_to_point_.end()) {
        return it->second;
    }
    return nullptr;
}

world::GridPoint* WarpedScreenGrid::pick_nearest_point(SDL_Point screen_pt, float max_distance_px) {
    float best_dist2 = max_distance_px * max_distance_px;
    world::GridPoint* best = nullptr;
    for (world::GridPoint* gp : visible_points_) {
        if (!gp) continue;
        const float dx = gp->screen.x - static_cast<float>(screen_pt.x);
        const float dy = gp->screen.y - static_cast<float>(screen_pt.y);
        const float dist2 = dx * dx + dy * dy;
        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            best = gp;
        }
    }
    return best;
}

WarpedScreenGrid::RenderSmoothingKey::RenderSmoothingKey(const Asset* asset, int frame)
    : asset_id(asset
        ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(asset))
        : 0),
      frame_index(frame) {}

void WarpedScreenGrid::set_focus_override(SDL_Point focus) {
    camera_.set_focus_override(focus);
}

void WarpedScreenGrid::clear_focus_override() {
    camera_.clear_focus_override();
}





double WarpedScreenGrid::default_camera_height_for_room(const Room* room) const {
    return std::max(1.0, compute_room_scale_from_area(room));
}

void WarpedScreenGrid::project_to_screen(world::GridPoint& point) const {

    const CameraController::State& cam_settings = camera_.state();
    const CameraState cam_state = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, cam_settings.center, cam_settings.params);
    const ProjectionResult proj = project_world_point_internal(
        cam_state,
        static_cast<double>(point.world_x()),
        static_cast<double>(point.world_y()),
        static_cast<double>(point.world_z()),
        screen_width_,
        screen_height_,
        horizon_fade_for_height(cam_state.camera_height));

    if (!proj.valid) {
        point.screen = SDL_FPoint{0.0f, 0.0f};
        point.parallax_dx = 0.0f;
        point.on_screen = false;
        return;
    }

    point.screen          = proj.screen;
    point.screen.y        = screen_height_ - point.screen.y;
    point.parallax_dx     = 0.0f;
    point.vertical_scale  = proj.vertical_scale;
    point.perspective_scale = proj.perspective_scale;
    point.horizon_fade_alpha = proj.horizon_fade;
    point.near_camera_fade_alpha = proj.near_camera_fade;
    point.distance_to_camera = proj.distance;
    point.tilt_radians    = static_cast<float>(runtime_pitch_rad_);
    point.on_screen       = true;
}

bool WarpedScreenGrid::is_manual_height_override() const {
    return camera_.manual_height_override();
}

void WarpedScreenGrid::set_manual_height_override(bool v) {
    camera_.set_manual_height_override(v);
}

double WarpedScreenGrid::get_scale() const {
    return std::max(1.0, camera_.current_height());
}

void WarpedScreenGrid::set_scale(double s) {
    const double clamped = std::max(1.0, s);
    CameraParams params = camera_.state().params;
    params.height_px = clamped;
    camera_.set_params(params);
    runtime_camera_height_ = camera_.current_height();
}

double WarpedScreenGrid::camera_y_distance() const {
    return camera_.state().params.y_distance_px;
}

void WarpedScreenGrid::set_camera_y_distance(double distance) {
    CameraParams params = camera_.state().params;
    params.y_distance_px = distance;
    camera_.set_params(params);
}

void WarpedScreenGrid::set_tilt_override(std::optional<float> tilt_deg) {
    if (!tilt_deg.has_value() || !std::isfinite(*tilt_deg)) {
        tilt_override_deg_.reset();
        return;
    }
    tilt_override_deg_ = camera_math::sanitize_pitch_degrees(*tilt_deg);
}

void WarpedScreenGrid::clear_tilt_override() {
    tilt_override_deg_.reset();
}

std::optional<float> WarpedScreenGrid::tilt_override() const {
    return tilt_override_deg_;
}

void WarpedScreenGrid::update() {
    camera_.tick(0.0f);
    runtime_camera_height_ = camera_.current_height();
    runtime_pitch_deg_ = camera_.current_pitch_deg();
    runtime_pitch_rad_ = camera_.current_pitch_rad();
    runtime_anchor_world_y_ = camera_.state().center.y;
}

Area WarpedScreenGrid::frame_to_area(const SDL_Rect& frame) const {
    SDL_FPoint tl = screen_to_map({frame.x, frame.y});
    SDL_FPoint tr = screen_to_map({frame.x + frame.w, frame.y});
    SDL_FPoint br = screen_to_map({frame.x + frame.w, frame.y + frame.h});
    SDL_FPoint bl = screen_to_map({frame.x, frame.y + frame.h});
    std::vector<Area::Point> corners{
        {static_cast<int>(tl.x), static_cast<int>(tl.y)},
        {static_cast<int>(tr.x), static_cast<int>(tr.y)},
        {static_cast<int>(br.x), static_cast<int>(br.y)},
        {static_cast<int>(bl.x), static_cast<int>(bl.y)}
    };
    return Area("frame_area", corners, 0);
}

SDL_Point WarpedScreenGrid::pan_and_height_to_point(double pan, double height) const {
    // Pan as x offset, height as y offset from center
    SDL_Point center = get_screen_center();
    return {center.x + static_cast<int>(pan), center.y + static_cast<int>(height)};
}

void WarpedScreenGrid::animate_height_multiply(double factor) {
    camera_.animate_height_multiply(factor);
}

void WarpedScreenGrid::animate_height_towards_point(double target_height, SDL_Point target_point) {
    camera_.animate_height_towards_point(target_height, target_point);
}

bool WarpedScreenGrid::is_height_animating() const {
    return camera_.is_animating();
}

SDL_Point WarpedScreenGrid::pan_and_height_to_asset(double pan, double height, const Asset* asset) const {
    if (!asset) return {0, 0};
    // Adjust asset position by pan and height
    return {asset->pos.x + static_cast<int>(pan), asset->pos.y + static_cast<int>(height)};
}
