#include "warped_screen_grid.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_types.hpp"
#include "utils/area.hpp"
#include "gameplay/map_generation/room.hpp"
#include "core/find_current_room.hpp"
#include "utils/log.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/world/chunk.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/screen_space_math.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "utils/frame_stats_recorder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>
#include <tuple>
#include <string>
#include <limits>
#include <optional>
#include <deque>
#include <cstdlib>
#include <sstream>
#include <nlohmann/json.hpp>

template <typename T>
T lerp(T a, T b, double t) {
    return static_cast<T>(a + (b - a) * t);
}

namespace {
    constexpr double PI_D       = 3.14159265358979323846;
    constexpr double kHalfFovY  = PI_D / 4.0;
    constexpr double kWorldUnitScale = 0.01;
    constexpr float  kHorizontalCullOverscanRatio = 0.20f;
    constexpr float  kBottomCullOverscanRatio = 0.40f;
    constexpr float  kNearHorizonSampleOffsetRatio = 0.02f;
    constexpr float  kMinNearHorizonSampleOffsetPx = 8.0f;
    constexpr float  kCameraMovementSpeedEpsilon = 1.0f;
    constexpr float  kLookAheadTimeSeconds = 0.12f;

    bool render_profiler_enabled() {
        static const bool enabled = [](){ const char* e=std::getenv("ENGINE_RENDER_GRID_PROFILER"); return e && std::string(e)!="0"; }();
        return enabled;
    }
    double render_profiler_spike_ms() {
        static const double threshold = [](){ const char* e=std::getenv("ENGINE_RENDER_GRID_SPIKE_MS"); return e ? std::max(0.0, std::atof(e)) : 16.0; }();
        return threshold;
    }
    struct RollingCosts { std::deque<double> values; };
    RollingCosts& rolling_costs(){ static RollingCosts r; return r; }
    double percentile(const std::deque<double>& v, double p){ if(v.empty()) return 0.0; std::vector<double> t(v.begin(),v.end()); std::sort(t.begin(),t.end()); size_t idx=std::min(t.size()-1, static_cast<size_t>(std::floor((p/100.0)*(t.size()-1)))); return t[idx]; }

    bool keep_dynamic_asset_for_visibility(const WarpedScreenGrid::RealismSettings& settings,
                                           const Asset& asset,
                                           double depth_distance) {
        if (!asset.is_dynamic_spawned_asset()) {
            return true;
        }
        if (!std::isfinite(depth_distance)) {
            return false;
        }
        const render_depth::DynamicDepthBand band = render_depth::classify_dynamic_depth_band(
            depth_distance,
            static_cast<double>(settings.dynamic_renderer_depth_efficiency_depth),
            static_cast<double>(settings.max_cull_depth));
        return band != render_depth::DynamicDepthBand::Culled;
    }

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

// clamp_height_scale וכל לוגיקה מבוססת קנה-מידה הוסרו; המצלמה כעת מפורשת לחלוטין לכל חדר.

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

}
namespace {
    SDL_FPoint ndc_to_screen_point(const CameraState& cam,
                                   double ndc_x,
                                   double ndc_y,
                                   int screen_width,
                                   int screen_height) {
        return render::screen_space::ndc_to_screen(
            ndc_x, ndc_y, screen_width, screen_height, cam.screen_zoom, cam.screen_pan_y_px);
    }

    std::pair<double, double> screen_to_ndc_point(const CameraState& cam,
                                                  double screen_x,
                                                  double screen_y,
                                                  int screen_width,
                                                  int screen_height) {
        (void)cam.inv_screen_zoom;
        return render::screen_space::screen_to_ndc(
            screen_x, screen_y, screen_width, screen_height, cam.screen_zoom, cam.screen_pan_y_px);
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
        const float extra_lr = safe_w * kHorizontalCullOverscanRatio;
        const float extra_down = safe_h * kBottomCullOverscanRatio;
        return ScreenBounds{
            -extra_lr,
            0.0f,
            safe_w + extra_lr,
            safe_h + extra_down
        };
    }

    bool point_inside_screen_bounds(const ScreenBounds& bounds, const SDL_FPoint& point) {
        return std::isfinite(point.x) &&
               std::isfinite(point.y) &&
               point.x >= bounds.left &&
               point.x <= bounds.right &&
               point.y >= bounds.top &&
               point.y <= bounds.bottom;
    }

    bool rect_intersects_screen_bounds(const ScreenBounds& bounds,
                                       float left,
                                       float top,
                                       float right,
                                       float bottom) {
        if (!std::isfinite(left) || !std::isfinite(top) ||
            !std::isfinite(right) || !std::isfinite(bottom)) {
            return false;
        }
        if (right < left || bottom < top) {
            return false;
        }
        const bool overlap_x = right >= bounds.left && left <= bounds.right;
        const bool overlap_y = bottom >= bounds.top && top <= bounds.bottom;
        return overlap_x && overlap_y;
    }


    bool rect_contains_screen_point(const ScreenBounds& bounds,
                                    float x,
                                    float y,
                                    float margin) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(margin)) {
            return false;
        }
        const float clamped_margin = std::max(0.0f, margin);
        return x >= (bounds.left - clamped_margin) &&
               x <= (bounds.right + clamped_margin) &&
               y >= (bounds.top - clamped_margin) &&
               y <= (bounds.bottom + clamped_margin);
    }

    float horizon_fade_for_height(double camera_height) {
        const double safe_height = std::max(1.0, camera_height);
        const double scaled = std::clamp(std::sqrt(safe_height) * 18.0, 60.0, 420.0);
        return static_cast<float>(scaled);
    }

    std::optional<SDL_FPoint> project_camera_space_point(const CameraState& cam,
                                                         double cam_x,
                                                         double cam_y,
                                                         double depth,
                                                         int screen_width,
                                                         int screen_height);

    float compute_raw_perspective_scale_from_depth(const CameraState& cam,
                                                   double depth_along_forward,
                                                   int screen_width);

    float clamp_perspective_scale_to_camera_edge(const CameraState& cam, float scale);

    double compute_min_perspective_depth_for_lower_edge(const CameraState& cam,
                                                        int screen_width,
                                                        int screen_height);

CameraState build_camera_state(const WarpedScreenGrid::RealismSettings& settings,
                                       double aspect,
                                   int screen_width,
                                   int screen_height,
                                   SDL_FPoint anchor_world,
                                   const CameraParams& params,
                                   bool lock_anchor_to_screen_center) {
        CameraState state{};

        const CameraParams safe_params =
            camera_math::sanitize_camera_params(params, settings.base_height_px);

        const double camera_height_pixels = safe_params.height_px;
        const float  tilt_deg             = camera_math::sanitize_pitch_degrees(static_cast<float>(safe_params.tilt_deg));

        const double pitch_rad = static_cast<double>(tilt_deg) * PI_D / 180.0;
        const double meters_scale = std::max(1e-6, kWorldUnitScale);
        const Vec3 anchor{ 0.0, 0.0, 0.0 };
    // גובה לאורך +Y (למעלה), עומק לאורך +Z (קדימה)
        Vec3 camera_pos{
            0.0,
            camera_height_pixels * meters_scale,
            0.0
        };
        state.screen_zoom = 1.0 + std::clamp(safe_params.zoom_percent, 0.0, 100.0) * 0.01;
        if (!std::isfinite(state.screen_zoom) || state.screen_zoom <= 0.0) {
            state.screen_zoom = 1.0;
        }
        state.inv_screen_zoom = 1.0 / state.screen_zoom;

        if (lock_anchor_to_screen_center) {
            const double tan_pitch = std::tan(pitch_rad);
            if (std::isfinite(tan_pitch) && std::fabs(tan_pitch) > 1e-6) {
    // הזז לאורך -Z כדי שהעוגן יישאר ממורכז בהתאם להטיה.
                camera_pos.z = camera_pos.y / tan_pitch;
            }
        }

        Vec3 to_anchor = anchor - camera_pos;
        Vec3 horiz_dir{ to_anchor.x, 0.0, to_anchor.z };
        const double horiz_len = length(horiz_dir);
        if (horiz_len < 1e-3 || !std::isfinite(horiz_len)) {
    // ברירת מחדל: מבט קדימה לאורך -Z כדי שהמצלמה תישב מאחורי העוגן.
            horiz_dir = Vec3{0.0, 0.0, -1.0};
        } else {
            horiz_dir = horiz_dir * (1.0 / horiz_len);
        }

        const double cos_pitch = std::cos(pitch_rad);
        const double sin_pitch = std::sin(pitch_rad);
        Vec3 forward{
            horiz_dir.x * cos_pitch,
            -sin_pitch,
            horiz_dir.z * cos_pitch
        };
        forward = normalize(forward);
        if (length(forward) < 1e-6 || !std::isfinite(length(forward))) {
            forward = Vec3{0.0, -sin_pitch, -cos_pitch};
            forward = normalize(forward);
        }

        Vec3 world_up{0.0, 1.0, 0.0};
    // בנה בסיס מצלמה ימני שבו +X נשאר לימין המסך.
        Vec3 right = cross(forward, world_up);
        right = normalize(right);
        Vec3 up = normalize(cross(right, forward));

        const double dist_horiz = length(Vec3{ anchor.x - camera_pos.x, 0.0, anchor.z - camera_pos.z });

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
        state.camera_world_y = camera_pos.y;
    state.anchor_world_y = 0.0; // עוגן גובה (Y)
    state.anchor_world_z = static_cast<double>(anchor_world.y); // עוגן עומק (Z)

        state.min_perspective_depth = compute_min_perspective_depth_for_lower_edge(
            state, screen_width, screen_height);
        state.max_perspective_scale = compute_raw_perspective_scale_from_depth(
            state, state.min_perspective_depth, screen_width);
        if (!std::isfinite(state.max_perspective_scale) || state.max_perspective_scale <= 0.0f) {
            state.max_perspective_scale = 1.0f;
        }

        return state;
    }

struct ProjectionResult {
        bool        valid = false;
        SDL_FPoint  screen{0.0f, 0.0f};
        float       perspective_scale = 1.0f;
        float       vertical_scale    = 1.0f;
        float       horizon_fade      = 1.0f;
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

    float compute_raw_perspective_scale_from_depth(const CameraState& cam,
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
        return result;
    }

    float clamp_perspective_scale_to_camera_edge(const CameraState& cam, float scale) {
        if (!std::isfinite(scale) || scale <= 0.0f) {
            return 1.0f;
        }
        if (std::isfinite(cam.max_perspective_scale) && cam.max_perspective_scale > 0.0f) {
            return std::clamp(scale, 0.0001f, cam.max_perspective_scale);
        }
        return std::max(0.0001f, scale);
    }

    double compute_min_perspective_depth_for_lower_edge(const CameraState& cam,
                                                        int screen_width,
                                                        int screen_height) {
        const double fallback_depth = std::max(cam.near_plane * 4.0, cam.focus_depth);
        if (!cam.valid || screen_width <= 0 || screen_height <= 0 ||
            !std::isfinite(cam.position.y) ||
            !std::isfinite(cam.forward.y) ||
            !std::isfinite(cam.up.y) ||
            !std::isfinite(cam.tan_half_fov_y)) {
            return fallback_depth;
        }

        const ScreenBounds bounds = expanded_screen_bounds(screen_width, screen_height);
        const double sample_x =
            (static_cast<double>(bounds.left) + static_cast<double>(bounds.right)) * 0.5;
        const auto [ndc_x, ndc_y] = screen_to_ndc_point(
            cam, sample_x, static_cast<double>(bounds.bottom), screen_width, screen_height);
        (void)ndc_x;
        if (!std::isfinite(ndc_y)) {
            return fallback_depth;
        }

        const double denom = cam.forward.y + ndc_y * cam.tan_half_fov_y * cam.up.y;
        if (!std::isfinite(denom) || std::fabs(denom) <= 1e-9) {
            return fallback_depth;
        }

        const double depth = -cam.position.y / denom;
        if (!std::isfinite(depth) || depth <= cam.near_plane) {
            return fallback_depth;
        }
        return std::max(depth, cam.near_plane * 1.25);
    }

    SDL_FPoint lerp_point(const SDL_FPoint& a, const SDL_FPoint& b, float t) {
        return SDL_FPoint{
            static_cast<float>(a.x + (b.x - a.x) * t),
            static_cast<float>(a.y + (b.y - a.y) * t)
        };
    }

    SDL_FPoint clamp_vector_magnitude(SDL_FPoint value, float max_magnitude) {
        if (!std::isfinite(value.x) || !std::isfinite(value.y)) {
            return SDL_FPoint{0.0f, 0.0f};
        }
        if (!std::isfinite(max_magnitude) || max_magnitude <= 0.0f) {
            return value;
        }
        const float len = std::sqrt(value.x * value.x + value.y * value.y);
        if (len <= max_magnitude || len <= 1.0e-6f) {
            return value;
        }
        const float scale = max_magnitude / len;
        return SDL_FPoint{value.x * scale, value.y * scale};
    }

    SDL_FPoint safe_normalized(SDL_FPoint value) {
        if (!std::isfinite(value.x) || !std::isfinite(value.y)) {
            return SDL_FPoint{0.0f, 0.0f};
        }
        const float len = std::sqrt(value.x * value.x + value.y * value.y);
        if (len <= 1.0e-6f) {
            return SDL_FPoint{0.0f, 0.0f};
        }
        return SDL_FPoint{value.x / len, value.y / len};
    }

    bool camera_transition_trace_enabled() {
        static const bool enabled = [] {
            const char* raw = SDL_getenv("VIBBLE_CAMERA_TRANSITION_TRACE");
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
            (world_y_pixels - cam.anchor_world_y) * safe_scale,
            (world_z_pixels - cam.anchor_world_z) * safe_scale
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

        float perspective_scale = compute_raw_perspective_scale_from_depth(
            cam, depth_along_forward, screen_width);
        float vertical_scale = 1.0f;

        const double unit_meters = std::max(1e-6, cam.meters_scale);
        const Vec3 unit_world_x{ unit_meters, 0.0, 0.0 };
        const Vec3 unit_world_y{ 0.0, unit_meters, 0.0 };
        const Vec3 delta_cam_x{
            dot(unit_world_x, cam.right),
            dot(unit_world_x, cam.up),
            dot(unit_world_x, cam.forward)
        };
        const Vec3 delta_cam_y{
            dot(unit_world_y, cam.right),
            dot(unit_world_y, cam.up),
            dot(unit_world_y, cam.forward)
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
        if (const auto scale_y = screen_distance_for_delta(delta_cam_y)) {
            if (std::isfinite(perspective_scale) && perspective_scale > 1e-6f) {
                vertical_scale = static_cast<float>(*scale_y / perspective_scale);
            }
        }
        if (!std::isfinite(perspective_scale) || perspective_scale <= 0.0f) {
            perspective_scale = 1.0f;
        }
        perspective_scale = clamp_perspective_scale_to_camera_edge(cam, perspective_scale);
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

        out.valid             = std::isfinite(screen_x) && std::isfinite(screen_y);
        out.screen            = SDL_FPoint{ static_cast<float>(screen_x), static_cast<float>(screen_y) };
        out.perspective_scale = perspective_scale;
        out.vertical_scale    = vertical_scale;
        out.horizon_fade      = horizon_fade;
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
        if (std::abs(dir.y) < 1e-6) {
            return std::nullopt;
        }
        const double t = -cam.position.y / dir.y;
        if (!std::isfinite(t) || t <= 0.0) {
            return std::nullopt;
        }
        const Vec3 hit = cam.position + dir * t;
        const double safe_scale = std::max(1e-6, cam.meters_scale);
        return SDL_FPoint{
            static_cast<float>(hit.x / safe_scale + static_cast<double>(cam.anchor_world_px.x)),
            static_cast<float>(hit.z / safe_scale + cam.anchor_world_z)
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

    world::CameraProjectionParams camera_state_to_projection_params(
        const CameraState& cam,
        int screen_width,
        int screen_height,
        float horizon_band_px) {
        world::CameraProjectionParams params{};
        params.position_x = cam.position.x;
        params.position_y = cam.position.y;
        params.position_z = cam.position.z;
        params.forward_x = cam.forward.x;
        params.forward_y = cam.forward.y;
        params.forward_z = cam.forward.z;
        params.right_x = cam.right.x;
        params.right_y = cam.right.y;
        params.right_z = cam.right.z;
        params.up_x = cam.up.x;
        params.up_y = cam.up.y;
        params.up_z = cam.up.z;
        params.anchor_world_x = static_cast<double>(cam.anchor_world_px.x);
    params.anchor_world_y = cam.anchor_world_y; // עוגן גובה
    params.anchor_world_z = cam.anchor_world_z; // עוגן עומק
        params.meters_scale = cam.meters_scale;
        params.tan_half_fov_x = cam.tan_half_fov_x;
        params.tan_half_fov_y = cam.tan_half_fov_y;
        params.near_plane = cam.near_plane;
        params.far_plane = cam.far_plane;
        params.min_perspective_depth = cam.min_perspective_depth;
        params.max_perspective_scale = cam.max_perspective_scale;
        params.screen_zoom = cam.screen_zoom;
        params.screen_pan_y_px = cam.screen_pan_y_px;
        params.horizon_screen_y = cam.horizon_screen_y;
        params.pitch_radians = cam.pitch_radians;
        params.horizon_band_px = horizon_band_px;
        params.screen_width = screen_width;
        params.screen_height = screen_height;
    params.state_version = 0; // ייקבע על ידי הקורא
        return params;
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
    display_view_ = adjusted_start;

    CameraParams initial_params;
    initial_params.height_px = settings_.base_height_px;
    initial_params.tilt_deg = camera_math::kDefaultCameraTiltDeg;
    camera_.reset(initial_params, start_center, settings_.base_height_px);
    camera_.set_transition_damping(transition_settings_.transition_damping);
    camera_.set_max_camera_velocity(transition_settings_.max_camera_velocity);
    camera_.set_transition_debug_state(static_cast<int>(CameraTransitionState::Idle), 0.0f);
    transition_telemetry_.state = CameraTransitionState::Idle;
    transition_telemetry_.target = SDL_FPoint{
        static_cast<float>(start_center.x),
        static_cast<float>(start_center.y)
    };
    transition_telemetry_.velocity = SDL_FPoint{0.0f, 0.0f};
    transition_telemetry_.blend_factor = 0.0f;
    transition_telemetry_.settle_time_remaining = 0.0f;
    runtime_camera_height_ = camera_.current_height();
    runtime_pitch_deg_ = camera_.current_pitch_deg();
    runtime_pitch_rad_ = camera_.current_pitch_rad();
    runtime_anchor_world_z_ = camera_state_cached().anchor_world_z;
    const ScreenBounds initial_bounds = expanded_screen_bounds(screen_width_, screen_height_);
    bounds_.left = initial_bounds.left;
    bounds_.top = initial_bounds.top;
    bounds_.right = initial_bounds.right;
    bounds_.bottom = initial_bounds.bottom;
    depth_enabled_   = true;
}

WarpedScreenGrid::~WarpedScreenGrid() = default;

const char* WarpedScreenGrid::transition_state_name(CameraTransitionState state) {
    switch (state) {
        case CameraTransitionState::Idle:
            return "סרק";
        case CameraTransitionState::BlendingToNewRoom:
            return "מיזוג_לחדר_חדש";
        case CameraTransitionState::Settling:
            return "התייצבות";
        default:
            return "לא_ידוע";
    }
}

std::uint64_t WarpedScreenGrid::camera_state_version() const {
    (void)camera_state_cached();
    return camera_state_version_;
}

void WarpedScreenGrid::set_frustum_padding_world(float padding) {
    frustum_padding_world_ = std::max(0.0f, padding);
}

void WarpedScreenGrid::set_realism_settings(const RealismSettings& settings) {
    settings_ = settings;
    settings_.min_visible_screen_ratio = std::clamp(settings_.min_visible_screen_ratio, 0.0f, 0.5f);
    settings_.boundary_min_visible_screen_ratio =
        std::clamp(settings_.boundary_min_visible_screen_ratio, 0.0f, 0.5f);
    if (!std::isfinite(settings_.base_height_px) || settings_.base_height_px <= 0.0f) {
        settings_.base_height_px = 720.0f;
    }
    if (!std::isfinite(settings_.max_cull_depth) || settings_.max_cull_depth < 1.0f) {
        settings_.max_cull_depth = 1.0f;
    }
    if (!std::isfinite(settings_.light_max_cull_depth) || settings_.light_max_cull_depth < settings_.max_cull_depth) {
        settings_.light_max_cull_depth = settings_.max_cull_depth;
    }
    if (!std::isfinite(settings_.dynamic_renderer_depth_efficiency_depth)) {
        settings_.dynamic_renderer_depth_efficiency_depth = 2000.0f;
    }
    settings_.dynamic_renderer_depth_efficiency_depth =
        std::clamp(settings_.dynamic_renderer_depth_efficiency_depth, 0.0f, settings_.max_cull_depth);
    if (!std::isfinite(settings_.dynamic_renderer_depth_efficiency_min_density_ratio)) {
        settings_.dynamic_renderer_depth_efficiency_min_density_ratio = 0.10f;
    }
    settings_.dynamic_renderer_depth_efficiency_min_density_ratio =
        std::clamp(settings_.dynamic_renderer_depth_efficiency_min_density_ratio, 0.0f, 1.0f);
    if (!std::isfinite(settings_.layer_depth_interval) || settings_.layer_depth_interval < 1.0f) {
        settings_.layer_depth_interval = 1.0f;
    }
    settings_.layer_depth_interval = std::min(settings_.layer_depth_interval, 100000.0f);
    if (!std::isfinite(settings_.layer_depth_curve) || settings_.layer_depth_curve < 0.0f) {
        settings_.layer_depth_curve = 0.0f;
    }
    settings_.layer_depth_curve = std::min(settings_.layer_depth_curve, 200.0f);
    if (!std::isfinite(settings_.blur_px) || settings_.blur_px < 0.0f) {
        settings_.blur_px = 0.0f;
    }
    settings_.blur_px = std::min(settings_.blur_px, 128.0f);
    if (!std::isfinite(settings_.radial_blur_px) || settings_.radial_blur_px < 0.0f) {
        settings_.radial_blur_px = 0.0f;
    }
    settings_.radial_blur_px = std::min(settings_.radial_blur_px, 256.0f);
    camera_.set_fallback_height(settings_.base_height_px);
    invalidate_camera_cache();

    // No-op: מטמון הגאומטריה אינו בשימוש עוד.
}

void WarpedScreenGrid::set_screen_center(SDL_Point p, bool snap_immediately) {
    const SDL_FPoint before_center = camera_.state().center;
    camera_.set_screen_center(p, snap_immediately);
    const SDL_FPoint after_center = camera_.state().center;
    const bool center_changed =
        std::fabs(after_center.x - before_center.x) > 1e-4f ||
        std::fabs(after_center.y - before_center.y) > 1e-4f;
    if (center_changed) {
        invalidate_camera_cache();
    }
}

void WarpedScreenGrid::set_screen_dimensions(int screen_width, int screen_height) {
    const int safe_w = std::max(1, screen_width);
    const int safe_h = std::max(1, screen_height);
    if (safe_w == screen_width_ && safe_h == screen_height_) {
        return;
    }

    screen_width_ = safe_w;
    screen_height_ = safe_h;
    aspect_ = static_cast<double>(screen_width_) / static_cast<double>(screen_height_);

    const SDL_Point center = get_screen_center();
    base_view_ = make_rect_area("base_view", center, screen_width_, screen_height_, 0);
    const ScreenBounds resized_bounds = expanded_screen_bounds(screen_width_, screen_height_);
    bounds_.left = resized_bounds.left;
    bounds_.top = resized_bounds.top;
    bounds_.right = resized_bounds.right;
    bounds_.bottom = resized_bounds.bottom;
    invalidate_camera_cache();
    recompute_current_view();
}

const CameraState& WarpedScreenGrid::camera_state_cached() const {
    const CameraController::State& controller_state = camera_.state();
    const CameraParams sanitized_params =
        camera_math::sanitize_camera_params(controller_state.params, settings_.base_height_px);
    auto quantize = [](double value) -> std::int64_t {
        if (!std::isfinite(value)) {
            return 0;
        }
        return static_cast<std::int64_t>(std::llround(value * 1000.0));
    };

    ProjectionFingerprint fingerprint{};
    fingerprint.center_x_q = quantize(static_cast<double>(controller_state.center.x));
    fingerprint.center_y_q = quantize(static_cast<double>(controller_state.center.y));
    fingerprint.height_px_q = quantize(sanitized_params.height_px);
    fingerprint.tilt_deg_q = quantize(sanitized_params.tilt_deg);
    fingerprint.zoom_percent_q = quantize(sanitized_params.zoom_percent);
    fingerprint.pan_y_px_q = quantize(0.0);
    fingerprint.aspect_q = quantize(aspect_);
    fingerprint.screen_width = screen_width_;
    fingerprint.screen_height = screen_height_;
    fingerprint.lock_anchor_to_center = lock_anchor_to_screen_center_;
    fingerprint.depth_enabled = depth_enabled_;
    fingerprint.has_tilt_override = tilt_override_deg_.has_value();
    fingerprint.tilt_override_q = quantize(tilt_override_deg_.value_or(camera_math::kDefaultCameraTiltDeg));

    const bool fingerprint_changed =
        !last_projection_fingerprint_.has_value() ||
        !(last_projection_fingerprint_.value() == fingerprint);

    if (cached_camera_state_dirty_ || !cached_camera_state_ || fingerprint_changed) {
        cached_camera_state_ = std::make_unique<CameraState>(build_camera_state(
            settings_,
            aspect_,
            screen_width_,
            screen_height_,
            controller_state.center,
            controller_state.params,
            lock_anchor_to_screen_center_));
        cached_camera_state_dirty_ = false;
        last_projection_fingerprint_ = fingerprint;
        if (fingerprint_changed) {
            ++camera_state_version_;
            if (camera_state_version_ == 0) {
                ++camera_state_version_;
            }
        }
    }
    return *cached_camera_state_;
}

void WarpedScreenGrid::invalidate_camera_cache() {
    cached_camera_state_dirty_ = true;
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
    invalidate_camera_cache();
}

void WarpedScreenGrid::update_camera_height(Room* cur,
                         CurrentRoomFinder* finder,
                         Asset* player,
                         bool refresh_requested,
                         float dt,
                         bool dev_mode)
{
    // השאר את העוגן נעול למרכז המסך כדי שפראלקסת העומק תישאר יציבה על מישור הקרקע.
    lock_anchor_to_screen_center_ = true;
    tracked_player_asset_ = player;
    const float safe_dt = (std::isfinite(dt) && dt > 0.0f) ? std::min(dt, 0.1f) : (1.0f / 60.0f);

    CameraParams cur_params;
    CameraParams neigh_params;
    double t = 0.0;
    const double fallback_height = std::max(1.0, static_cast<double>(settings_.base_height_px));
    auto room_camera_center = [](const Room* room) -> SDL_Point {
        if (!room || !room->room_area) {
            return SDL_Point{0, 0};
        }
        const SDL_Point room_center = room->room_area->get_center();
        return SDL_Point{
            room_center.x + room->camera_center_dx,
            room_center.y + room->camera_center_dz
        };
    };

    if (!dev_mode) {
        if (camera_.manual_height_override()) {
            vibble::log::info("[Camera] מנקה manual_height_override במצב רגיל");
            camera_.set_manual_height_override(false);
        }
        if (camera_.manual_zoom_override()) {
            vibble::log::info("[Camera] מנקה manual_zoom_override במצב רגיל");
            camera_.set_manual_zoom_override(false);
        }
    }

    if (cur) {
        cur_params.height_px = cur->camera_height_px;
        cur_params.tilt_deg = cur->camera_tilt_deg;
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
        neigh_params.zoom_percent = neigh->camera_zoom_percent;
    }
    if (tilt_override_deg_ && dev_mode) {
        cur_params.tilt_deg = *tilt_override_deg_;
        neigh_params.tilt_deg = *tilt_override_deg_;
    }
    cur_params = camera_math::sanitize_camera_params(cur_params, fallback_height);
    neigh_params = camera_math::sanitize_camera_params(neigh_params, fallback_height);

    // מקדם אינטרפולציה t: אם השחקן בין חדרים בצע אינטרפולציה; אחרת השתמש ב-cur
    if (player && cur && cur->room_area && neigh && neigh->room_area && cur != neigh) {
        auto [ax, ay] = cur->room_area->get_center();
        auto [bx, by] = neigh->room_area->get_center();
        const double pax = double(player->world_x());
        const double pay = double(player->world_z());
        const double vx = double(bx - ax);
        const double vy = double(by - ay);
        const double wx = double(pax - ax);
        const double wy = double(pay - ay);
        const double vlen2 = vx * vx + vy * vy;
        t = (vlen2 > 0.0) ? ((wx * vx + wy * vy) / vlen2) : 0.0;
        t = std::clamp(t, 0.0, 1.0);
    }

    camera_.apply_room_targets(cur_params, neigh_params, t, refresh_requested, 20, dev_mode);

    const bool focus_override_active = camera_.has_focus_override();
    if (!dev_mode) {
        if (focus_override_active) {
            camera_.clear_focus_override();
        }
    }

    SDL_FPoint fallback_target = camera_.state().center;
    if (player) {
        fallback_target.x = static_cast<float>(player->world_x());
        fallback_target.y = static_cast<float>(player->world_z());
    } else if (cur && cur->room_area) {
        const SDL_Point rc = room_camera_center(cur);
        fallback_target.x = static_cast<float>(rc.x);
        fallback_target.y = static_cast<float>(rc.y);
    }
    const SDL_FPoint cur_room_target = (cur && cur->room_area)
        ? SDL_FPoint{static_cast<float>(room_camera_center(cur).x), static_cast<float>(room_camera_center(cur).y)}
        : fallback_target;
    const SDL_FPoint neigh_room_target = (neigh && neigh->room_area)
        ? SDL_FPoint{static_cast<float>(room_camera_center(neigh).x), static_cast<float>(room_camera_center(neigh).y)}
        : cur_room_target;
    const SDL_FPoint blended_room_target = lerp_point(cur_room_target, neigh_room_target, static_cast<float>(t));

    SDL_FPoint player_velocity{0.0f, 0.0f};
    bool player_moving = false;
    if (player) {
        const SDL_FPoint player_world{
            static_cast<float>(player->world_x()),
            static_cast<float>(player->world_z())
        };
        if (previous_player_world_valid_ && safe_dt > 0.0f) {
            player_velocity.x = (player_world.x - previous_player_world_.x) / safe_dt;
            player_velocity.y = (player_world.y - previous_player_world_.y) / safe_dt;
            const float speed = std::sqrt(player_velocity.x * player_velocity.x + player_velocity.y * player_velocity.y);
            player_moving = speed > kCameraMovementSpeedEpsilon;
        }
        previous_player_world_ = player_world;
        previous_player_world_valid_ = true;
    } else {
        previous_player_world_valid_ = false;
    }

    const bool room_changed = (previous_transition_room_ != nullptr && cur != previous_transition_room_);
    CameraTransitionState transition_state = transition_telemetry_.state;

    if (room_changed) {
        transition_state = CameraTransitionState::BlendingToNewRoom;
    }

    const float configured_settle = std::clamp(
        std::isfinite(transition_settings_.settle_duration_after_stop)
            ? transition_settings_.settle_duration_after_stop
            : 0.0f,
        0.0f,
        10.0f);

    const bool between_rooms = (cur && neigh && cur != neigh && t > 0.0 && t < 1.0);
    if (player) {
        if (player_moving) {
            settle_time_remaining_ = configured_settle;
            transition_state = between_rooms || room_changed
                ? CameraTransitionState::BlendingToNewRoom
                : CameraTransitionState::Idle;
        } else {
            if (previous_player_moving_) {
                settle_time_remaining_ = configured_settle;
                transition_state = CameraTransitionState::Settling;
            } else if (transition_state == CameraTransitionState::Settling && settle_time_remaining_ > 0.0f) {
                transition_state = CameraTransitionState::Settling;
            } else if (!between_rooms) {
                transition_state = CameraTransitionState::Idle;
            }
            if (transition_state == CameraTransitionState::Settling) {
                settle_time_remaining_ = std::max(0.0f, settle_time_remaining_ - safe_dt);
                if (settle_time_remaining_ <= 0.0f) {
                    transition_state = CameraTransitionState::Idle;
                }
            }
        }
    } else if (!between_rooms) {
        transition_state = CameraTransitionState::Idle;
        settle_time_remaining_ = 0.0f;
    }

    const float blend_damping_scale = std::clamp(
        std::isfinite(transition_settings_.room_blend_damping_scale)
            ? transition_settings_.room_blend_damping_scale
            : 0.14f,
        0.05f,
        1.0f);
    const float blend_velocity_scale = std::clamp(
        std::isfinite(transition_settings_.room_blend_velocity_scale)
            ? transition_settings_.room_blend_velocity_scale
            : 0.18f,
        0.10f,
        1.0f);
    const float blend_follow_scale = std::clamp(
        std::isfinite(transition_settings_.room_blend_follow_weight_scale)
            ? transition_settings_.room_blend_follow_weight_scale
            : 0.28f,
        0.05f,
        1.0f);
    const bool blending_state = (transition_state == CameraTransitionState::BlendingToNewRoom);
    const float effective_damping = transition_settings_.transition_damping *
        (blending_state ? blend_damping_scale : 1.0f);
    const float effective_max_velocity = transition_settings_.max_camera_velocity *
        (blending_state ? blend_velocity_scale : 1.0f);
    camera_.set_transition_damping(effective_damping);
    camera_.set_max_camera_velocity(effective_max_velocity);

    SDL_FPoint desired_center = blended_room_target;
    SDL_FPoint player_focus = blended_room_target;
    if (player) {
        player_focus = SDL_FPoint{
            static_cast<float>(player->world_x()),
            static_cast<float>(player->world_z())
        };
    }

    if (!dev_mode && player && player_moving) {
        const float look_ahead_weight = std::clamp(
            std::isfinite(transition_settings_.movement_look_ahead_weight)
                ? transition_settings_.movement_look_ahead_weight
                : 0.0f,
            0.0f,
            2.0f);
        SDL_FPoint look_ahead{
            player_velocity.x * kLookAheadTimeSeconds * look_ahead_weight,
            player_velocity.y * kLookAheadTimeSeconds * look_ahead_weight
        };
        const float look_ahead_limit = std::max(0.0f, transition_settings_.max_camera_velocity) * 0.35f;
        look_ahead = clamp_vector_magnitude(look_ahead, look_ahead_limit);
        player_focus.x += look_ahead.x;
        player_focus.y += look_ahead.y;
    }

    if (!dev_mode && player) {
        float follow_weight = std::clamp(
            std::isfinite(transition_settings_.player_follow_weight)
                ? transition_settings_.player_follow_weight
                : 0.35f,
            0.0f,
            1.0f);
        if (blending_state) {
            follow_weight *= blend_follow_scale;
        }
        desired_center = lerp_point(desired_center, player_focus, follow_weight);

        const float configured_soft_leash = std::clamp(
            std::isfinite(transition_settings_.player_soft_leash_px)
                ? transition_settings_.player_soft_leash_px
                : 220.0f,
            32.0f,
            5000.0f);
        const float configured_hard_leash = std::clamp(
            std::isfinite(transition_settings_.player_hard_leash_px)
                ? transition_settings_.player_hard_leash_px
                : 360.0f,
            configured_soft_leash + 1.0f,
            6000.0f);

        SDL_FPoint to_focus{
            player_focus.x - desired_center.x,
            player_focus.y - desired_center.y
        };
        float distance_to_focus = std::sqrt(to_focus.x * to_focus.x + to_focus.y * to_focus.y);
        if (distance_to_focus > configured_soft_leash) {
            const SDL_FPoint dir = safe_normalized(to_focus);
            const float excess = distance_to_focus - configured_soft_leash;
            const float catch_up = excess * 0.65f;
            desired_center.x += dir.x * catch_up;
            desired_center.y += dir.y * catch_up;
            to_focus.x = player_focus.x - desired_center.x;
            to_focus.y = player_focus.y - desired_center.y;
            distance_to_focus = std::sqrt(to_focus.x * to_focus.x + to_focus.y * to_focus.y);
        }

        if (distance_to_focus > configured_hard_leash) {
            const SDL_FPoint dir = safe_normalized(to_focus);
            desired_center.x = player_focus.x - dir.x * configured_hard_leash;
            desired_center.y = player_focus.y - dir.y * configured_hard_leash;
        }

        const float safe_x = std::min(
            configured_hard_leash,
            std::clamp(static_cast<float>(screen_width_) * 0.33f, 80.0f, 650.0f));
        const float safe_y = std::min(
            configured_hard_leash,
            std::clamp(static_cast<float>(screen_height_) * 0.27f, 60.0f, 500.0f));

        const float delta_x = player_focus.x - desired_center.x;
        const float delta_y = player_focus.y - desired_center.y;
        if (std::fabs(delta_x) > safe_x) {
            desired_center.x = player_focus.x - std::copysign(safe_x, delta_x);
        }
        if (std::fabs(delta_y) > safe_y) {
            desired_center.y = player_focus.y - std::copysign(safe_y, delta_y);
        }
    }

    if (dev_mode && focus_override_active) {
    // נתיב דיבאג מפורש: נעילת פוקוס צריכה לקפוץ בצורה קשיחה.
        set_screen_center(camera_.state().focus_override, true);
        transition_state = CameraTransitionState::Idle;
        settle_time_remaining_ = 0.0f;
        desired_center = SDL_FPoint{
            static_cast<float>(camera_.state().focus_override.x),
            static_cast<float>(camera_.state().focus_override.y)
        };
    } else {
        set_screen_center(SDL_Point{
            static_cast<int>(std::lround(desired_center.x)),
            static_cast<int>(std::lround(desired_center.y))
        }, false);
    }

    camera_.set_transition_debug_state(static_cast<int>(transition_state), static_cast<float>(t));
    camera_.tick(dt);
    const CameraController::State& controller_state = camera_.state();
    transition_telemetry_.state = transition_state;
    transition_telemetry_.target = desired_center;
    transition_telemetry_.velocity = controller_state.center_velocity;
    transition_telemetry_.blend_factor = static_cast<float>(t);
    transition_telemetry_.settle_time_remaining = settle_time_remaining_;

    if (camera_transition_trace_enabled()) {
        auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
        frame_stats.set("camera.transition_state", transition_state_name(transition_state));
        frame_stats.set("camera.transition_target_x", static_cast<double>(desired_center.x));
        frame_stats.set("camera.transition_target_y", static_cast<double>(desired_center.y));
        frame_stats.set("camera.transition_velocity_x", static_cast<double>(controller_state.center_velocity.x));
        frame_stats.set("camera.transition_velocity_y", static_cast<double>(controller_state.center_velocity.y));
        frame_stats.set("camera.transition_blend_factor", static_cast<double>(static_cast<float>(t)));
        frame_stats.set("camera.transition_settle_time_remaining", static_cast<double>(settle_time_remaining_));
    }
    if (false && camera_transition_trace_enabled()) {
        vibble::log::debug(
            std::string("[CameraTransition] מצב=") + transition_state_name(transition_state) +
            " מטרה=(" + std::to_string(desired_center.x) + "," + std::to_string(desired_center.y) + ")" +
            " מהירות=(" + std::to_string(controller_state.center_velocity.x) + "," +
            std::to_string(controller_state.center_velocity.y) + ")" +
            " מיזוג=" + std::to_string(static_cast<float>(t)) +
            " התייצבות_נותרה=" + std::to_string(settle_time_remaining_));
    }

    previous_transition_room_ = cur;
    previous_player_moving_ = player_moving;

    const CameraState& cam_state = camera_state_cached();
    runtime_camera_height_ = cam_state.camera_height;
    runtime_focus_depth_ = cam_state.focus_depth;
    runtime_anchor_world_z_ = cam_state.anchor_world_z;
    runtime_focus_ndc_offset_ = cam_state.focus_ndc_offset;
    runtime_pitch_rad_ = cam_state.pitch_radians;
    runtime_pitch_deg_ = cam_state.pitch_degrees;
    runtime_depth_offset_px_ = static_cast<float>(cam_state.reference_depth);
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
    const CameraState& cam = camera_state_cached();
    runtime_camera_height_ = cam.camera_height;
    runtime_focus_depth_ = cam.focus_depth;
    runtime_anchor_world_z_ = cam.anchor_world_z;
    runtime_focus_ndc_offset_ = cam.focus_ndc_offset;
    runtime_pitch_rad_ = cam.pitch_radians;
    runtime_pitch_deg_ = cam.pitch_degrees;
    runtime_depth_offset_px_ = static_cast<float>(cam.reference_depth);

    auto build_view_from_screen_bounds = [&](const ScreenBounds& screen_bounds,
                                             float world_padding,
                                             const char* view_name) -> Area {
        std::vector<SDL_FPoint> ground_points;
        ground_points.reserve(6);
        const float safe_h = static_cast<float>(std::max(1, screen_height_));
        const float near_horizon_sample_y = std::clamp(
            static_cast<float>(cam.horizon_screen_y) +
                std::max(kMinNearHorizonSampleOffsetPx, safe_h * kNearHorizonSampleOffsetRatio),
            screen_bounds.top,
            screen_bounds.bottom);
        const float mid_x = (screen_bounds.left + screen_bounds.right) * 0.5f;
        const std::array<SDL_FPoint, 6> screen_samples{
            SDL_FPoint{screen_bounds.left, near_horizon_sample_y},
            SDL_FPoint{mid_x, near_horizon_sample_y},
            SDL_FPoint{screen_bounds.right, near_horizon_sample_y},
            SDL_FPoint{screen_bounds.left, screen_bounds.bottom},
            SDL_FPoint{mid_x, screen_bounds.bottom},
            SDL_FPoint{screen_bounds.right, screen_bounds.bottom}
        };
        for (const auto& sample : screen_samples) {
            const auto [nx, ny] = screen_to_ndc_point(
                cam,
                static_cast<double>(sample.x),
                static_cast<double>(sample.y),
                screen_width_,
                screen_height_);
            auto gp = project_ndc_to_ground(cam, nx, ny);
            if (gp.has_value()) {
                ground_points.push_back(*gp);
            }
        }

        if (ground_points.empty()) {
            SDL_Point center{
                static_cast<int>(std::lround(cam.anchor_world_px.x)),
                static_cast<int>(std::lround(cam.anchor_world_px.y))
            };
            const int view_w =
                std::max(1, static_cast<int>(std::lround(screen_bounds.right - screen_bounds.left)));
            const int view_h =
                std::max(1, static_cast<int>(std::lround(screen_bounds.bottom - screen_bounds.top)));
            return make_rect_area(view_name, center, view_w, view_h, 0);
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

        minx -= world_padding;
        maxx += world_padding;
        miny -= world_padding;
        maxy += world_padding;

        const int view_w = std::max(1, static_cast<int>(std::lround(maxx - minx)));
        const int view_h = std::max(1, static_cast<int>(std::lround(maxy - miny)));
        SDL_Point center{
            static_cast<int>(std::lround((minx + maxx) * 0.5f)),
            static_cast<int>(std::lround((miny + maxy) * 0.5f))
        };
        return make_rect_area(view_name, center, view_w, view_h, 0);
    };

    const ScreenBounds strict_screen_bounds{
        0.0f,
        0.0f,
        static_cast<float>(std::max(1, screen_width_)),
        static_cast<float>(std::max(1, screen_height_))
    };
    const ScreenBounds virtual_bounds = expanded_screen_bounds(screen_width_, screen_height_);

    display_view_ = build_view_from_screen_bounds(strict_screen_bounds, 0.0f, "display_view");
    current_view_ = build_view_from_screen_bounds(
        virtual_bounds,
        std::max(0.0f, frustum_padding_world_),
        "current_view");
}



SDL_FPoint WarpedScreenGrid::map_to_screen(SDL_Point world) const {
    SDL_FPoint world_f{ static_cast<float>(world.x), static_cast<float>(world.y) };
    return map_to_screen_f(world_f);
}

SDL_FPoint WarpedScreenGrid::map_to_screen_f(SDL_FPoint world) const {
    const CameraState& cam = camera_state_cached();
    ProjectionResult proj = project_world_point_internal(cam,
                                                static_cast<double>(world.x),
        0.0, // ברירת מחדל לגובה היא 0
        static_cast<double>(world.y), // עומק
                                                screen_width_,
                                                screen_height_,
                                                horizon_fade_for_height(cam.camera_height));
    if (!proj.valid) {
        return SDL_FPoint{0.0f, 0.0f};
    }
    return proj.screen;
}

bool WarpedScreenGrid::project_world_point(SDL_FPoint world, float world_z, SDL_FPoint& out) const {
    const CameraState& cam = camera_state_cached();
    ProjectionResult proj = project_world_point_internal(cam,
                                                static_cast<double>(world.x),
        static_cast<double>(world.y),   // גובה (Y)
        static_cast<double>(world_z),   // עומק (Z)
                                                screen_width_,
                                                screen_height_,
                                                horizon_fade_for_height(cam.camera_height));
    if (!proj.valid) {
        return false;
    }
    out = proj.screen;
    return true;
}

bool WarpedScreenGrid::sample_perspective_scale(SDL_FPoint world, float world_z, float& out_scale) const {
    out_scale = 1.0f;
    const CameraState& cam = camera_state_cached();
    ProjectionResult proj = project_world_point_internal(cam,
                                                static_cast<double>(world.x),
        static_cast<double>(world.y),
        static_cast<double>(world_z),
                                                screen_width_,
                                                screen_height_,
                                                horizon_fade_for_height(cam.camera_height));
    if (!proj.valid || !std::isfinite(proj.perspective_scale) || proj.perspective_scale <= 0.0f) {
        return false;
    }
    out_scale = std::max(0.0001f, proj.perspective_scale);
    return true;
}

bool WarpedScreenGrid::build_camera_ray_from_screen(const SDL_FPoint& screen_point,
                                                     render_projection::CameraRay& out_ray) const {
    return render_projection::build_camera_ray_from_screen(projection_params(), screen_point, out_ray);
}

bool WarpedScreenGrid::screen_to_world_on_depth_plane(const SDL_FPoint& screen_point,
                                                      float world_z,
                                                      render_projection::WorldPoint3& out_world_point) const {
    render_projection::CameraRay ray{};
    if (!build_camera_ray_from_screen(screen_point, ray)) {
        return false;
    }
    return render_projection::intersect_camera_ray_on_world_z(projection_params(),
                                                              ray,
                                                              world_z,
                                                              out_world_point);
}

SDL_FPoint WarpedScreenGrid::screen_to_map(SDL_Point screen) const {
    const CameraState& cam = camera_state_cached();
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

    const CameraState& cam = camera_state_cached();
    ProjectionResult proj = project_world_point_internal(cam,
                                                static_cast<double>(world.x),
        static_cast<double>(world.y),   // גובה (Y)
        static_cast<double>(world_z),   // עומק (Z)
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
    result.horizon_fade_alpha = proj.horizon_fade;
    return result;
}

world::CameraProjectionParams WarpedScreenGrid::projection_params() const {
    const CameraState& cam_state = camera_state_cached();
    const float horizon_band = horizon_fade_for_height(cam_state.camera_height);
    world::CameraProjectionParams params = camera_state_to_projection_params(
        cam_state, screen_width_, screen_height_, horizon_band);
    params.state_version = camera_state_version_;
    return params;
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
    auto read_bool = [&](const char* key, bool& target) {
        auto it = data.find(key);
        if (it == data.end() || !it->is_boolean()) {
            return;
        }
        target = it->get<bool>();
    };
    auto read_transition_float = [&](const char* key, float fallback, float min_value, float max_value) {
        auto it = data.find(key);
        if (it == data.end() || !it->is_number()) {
            return fallback;
        }
        const float value = it->get<float>();
        if (!std::isfinite(value)) {
            return fallback;
        }
        return std::clamp(value, min_value, max_value);
    };
    RealismSettings updated = settings_;
    read_float("min_visible_screen_ratio", updated.min_visible_screen_ratio, 0.0f, 0.5f);
    read_float("boundary_min_visible_screen_ratio", updated.boundary_min_visible_screen_ratio, 0.0f, 0.5f);
    read_bool("min_visible_uses_light_radius", updated.min_visible_uses_light_radius);
    read_float("base_height_px", updated.base_height_px, 1.0f, 100000.0f);
    read_float("max_cull_depth", updated.max_cull_depth, 1.0f, 1000000.0f);
    read_float("light_max_cull_depth", updated.light_max_cull_depth, 1.0f, 1000000.0f);
    read_float("dynamic_renderer_depth_efficiency_depth",
               updated.dynamic_renderer_depth_efficiency_depth,
               0.0f,
               updated.max_cull_depth);
    read_float("dynamic_renderer_depth_efficiency_min_density_ratio",
               updated.dynamic_renderer_depth_efficiency_min_density_ratio,
               0.0f,
               1.0f);
    read_float("layer_depth_interval", updated.layer_depth_interval, 1.0f, 100000.0f);
    read_float("layer_depth_curve", updated.layer_depth_curve, 0.0f, 200.0f);
    read_bool("light_radius_overlap_culling_enabled", updated.light_radius_overlap_culling_enabled);
    read_float("blur_px", updated.blur_px, 0.0f, 128.0f);
    read_float("radial_blur_px", updated.radial_blur_px, 0.0f, 256.0f);
    read_bool("depth_of_field_enabled", updated.depth_of_field_enabled);
    set_realism_settings(updated);

    transition_settings_.transition_damping = read_transition_float(
        "transition_damping",
        transition_settings_.transition_damping,
        0.0f,
        200.0f);
    transition_settings_.max_camera_velocity = read_transition_float(
        "max_camera_velocity",
        transition_settings_.max_camera_velocity,
        1.0f,
        100000.0f);
    transition_settings_.room_blend_damping_scale = read_transition_float(
        "room_blend_damping_scale",
        transition_settings_.room_blend_damping_scale,
        0.05f,
        1.0f);
    transition_settings_.room_blend_velocity_scale = read_transition_float(
        "room_blend_velocity_scale",
        transition_settings_.room_blend_velocity_scale,
        0.10f,
        1.0f);
    transition_settings_.room_blend_follow_weight_scale = read_transition_float(
        "room_blend_follow_weight_scale",
        transition_settings_.room_blend_follow_weight_scale,
        0.05f,
        1.0f);
    transition_settings_.settle_duration_after_stop = read_transition_float(
        "settle_duration_after_stop",
        transition_settings_.settle_duration_after_stop,
        0.0f,
        10.0f);
    transition_settings_.movement_look_ahead_weight = read_transition_float(
        "movement_look_ahead_weight",
        transition_settings_.movement_look_ahead_weight,
        0.0f,
        2.0f);
    transition_settings_.player_follow_weight = read_transition_float(
        "player_follow_weight",
        transition_settings_.player_follow_weight,
        0.0f,
        1.0f);
    transition_settings_.player_soft_leash_px = read_transition_float(
        "player_soft_leash_px",
        transition_settings_.player_soft_leash_px,
        32.0f,
        5000.0f);
    transition_settings_.player_hard_leash_px = read_transition_float(
        "player_hard_leash_px",
        transition_settings_.player_hard_leash_px,
        transition_settings_.player_soft_leash_px + 1.0f,
        6000.0f);
    transition_settings_.player_hard_leash_px = std::max(
        transition_settings_.player_hard_leash_px,
        transition_settings_.player_soft_leash_px + 1.0f);

    camera_.set_transition_damping(transition_settings_.transition_damping);
    camera_.set_max_camera_velocity(transition_settings_.max_camera_velocity);
}

nlohmann::json WarpedScreenGrid::camera_settings_to_json() const {
    nlohmann::json result = nlohmann::json::object();
    result["min_visible_screen_ratio"] = settings_.min_visible_screen_ratio;
    result["boundary_min_visible_screen_ratio"] = settings_.boundary_min_visible_screen_ratio;
    result["min_visible_uses_light_radius"] = settings_.min_visible_uses_light_radius;
    result["base_height_px"] = settings_.base_height_px;
    result["max_cull_depth"] = settings_.max_cull_depth;
    result["light_max_cull_depth"] = settings_.light_max_cull_depth;
    result["dynamic_renderer_depth_efficiency_depth"] =
        settings_.dynamic_renderer_depth_efficiency_depth;
    result["dynamic_renderer_depth_efficiency_min_density_ratio"] =
        settings_.dynamic_renderer_depth_efficiency_min_density_ratio;
    result["layer_depth_interval"] = settings_.layer_depth_interval;
    result["layer_depth_curve"] = settings_.layer_depth_curve;
    result["light_radius_overlap_culling_enabled"] = settings_.light_radius_overlap_culling_enabled;
    result["blur_px"] = settings_.blur_px;
    result["radial_blur_px"] = settings_.radial_blur_px;
    result["depth_of_field_enabled"] = settings_.depth_of_field_enabled;
    result["transition_damping"] = transition_settings_.transition_damping;
    result["max_camera_velocity"] = transition_settings_.max_camera_velocity;
    result["room_blend_damping_scale"] = transition_settings_.room_blend_damping_scale;
    result["room_blend_velocity_scale"] = transition_settings_.room_blend_velocity_scale;
    result["room_blend_follow_weight_scale"] = transition_settings_.room_blend_follow_weight_scale;
    result["settle_duration_after_stop"] = transition_settings_.settle_duration_after_stop;
    result["movement_look_ahead_weight"] = transition_settings_.movement_look_ahead_weight;
    result["player_follow_weight"] = transition_settings_.player_follow_weight;
    result["player_soft_leash_px"] = transition_settings_.player_soft_leash_px;
    result["player_hard_leash_px"] = transition_settings_.player_hard_leash_px;
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
    const CameraState& cam = camera_state_cached();
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

double WarpedScreenGrid::focus_plane_world_z() const {
    return runtime_anchor_world_z_;
}


double WarpedScreenGrid::anchor_world_z() const {
    return focus_plane_world_z();
}

void WarpedScreenGrid::clear_grid_state() {
    warped_points_.clear();
    visible_traversal_entries_.clear();
    asset_to_point_.clear();
    visibility_reason_flags_.clear();
    cached_world_rect_ = SDL_Rect{0, 0, 0, 0};
    bounds_ = GridBounds{};
}

void WarpedScreenGrid::rebuild_grid_bounds() {
    if (warped_points_.empty()) {
        cached_world_rect_ = SDL_Rect{0, 0, 0, 0};
        bounds_ = GridBounds{};
        return;
    }

    auto compute_bounds = [](const std::vector<world::GridPoint*>& points, bool on_screen_only) {
        int min_x = std::numeric_limits<int>::max();
        int max_x = std::numeric_limits<int>::min();
        int min_z = std::numeric_limits<int>::max();
        int max_z = std::numeric_limits<int>::min();
        for (const auto* gp : points) {
            if (!gp) {
                continue;
            }
            if (on_screen_only && !gp->is_on_screen()) {
                continue;
            }
            min_x = std::min(min_x, gp->world_x());
            max_x = std::max(max_x, gp->world_x());
            min_z = std::min(min_z, gp->world_z());
            max_z = std::max(max_z, gp->world_z());
        }
        return std::array<int, 4>{min_x, max_x, min_z, max_z};
    };

    auto bounds = compute_bounds(warped_points_, true);
    int min_x = bounds[0];
    int max_x = bounds[1];
    int min_z = bounds[2];
    int max_z = bounds[3];

    if (min_x == std::numeric_limits<int>::max()) {
        bounds = compute_bounds(warped_points_, false);
        min_x = bounds[0];
        max_x = bounds[1];
        min_z = bounds[2];
        max_z = bounds[3];
    }

    if (min_x == std::numeric_limits<int>::max()) {
        cached_world_rect_ = SDL_Rect{0, 0, 0, 0};
        bounds_ = GridBounds{};
        return;
    }

    const int width  = std::max(0, max_x - min_x);
    const int height = std::max(0, max_z - min_z);
    cached_world_rect_ = SDL_Rect{min_x, min_z, width, height};

    bounds_.left   = static_cast<float>(min_x);
    bounds_.top    = static_cast<float>(min_z);
    bounds_.right  = static_cast<float>(max_x);
    bounds_.bottom = static_cast<float>(max_z);
}


void WarpedScreenGrid::rebuild_grid(world::WorldGrid& world_grid,
                                    float dt_seconds,
                                    std::uint64_t frame_id) {
    (void)dt_seconds;
    frame_counter_ = frame_id;
    const std::uint64_t frame_stamp = frame_id;
    clear_grid_state();
    const bool profiler_enabled = render_profiler_enabled();
    const std::uint64_t profiler_begin = profiler_enabled ? SDL_GetPerformanceCounter() : 0;
    const std::uint64_t profiler_freq = profiler_enabled ? SDL_GetPerformanceFrequency() : 0;
    auto ms_since = [&](std::uint64_t start)->double { if (!profiler_enabled || profiler_freq==0) return 0.0; return (static_cast<double>(SDL_GetPerformanceCounter()-start)*1000.0)/static_cast<double>(profiler_freq); };
    double phase_view_ms=0, phase_candidate_ms=0, phase_projection_ms=0, phase_visibility_ms=0, phase_traversal_ms=0;

    // רענן את אזור התצוגה במרחב העולם לפי פרמטרי המצלמה האחרונים.
    const std::uint64_t phase_view_begin = profiler_enabled ? SDL_GetPerformanceCounter() : 0;
    recompute_current_view();
    phase_view_ms = ms_since(phase_view_begin);

    int minx, miny, maxx, maxy;
    std::tie(minx, miny, maxx, maxy) = current_view_.get_bounds();
    const int width = std::max(0, maxx - minx);
    const int height = std::max(0, maxy - miny);
    const world::GridBounds world_bounds = world::GridBounds::from_xywh(
        minx,
        miny,
        width,
        height,
        0,
        world_grid.default_resolution_layer());
    const ScreenBounds cull_bounds = expanded_screen_bounds(screen_width_, screen_height_);
    const CameraState& cam_state = camera_state_cached();

    world::WorldGrid::RegionMetrics region_metrics{};
    const int min_world_z = std::numeric_limits<int>::min();
    const int max_world_z = static_cast<int>(std::ceil(
        cam_state.anchor_world_z + static_cast<double>(settings_.light_max_cull_depth)));
    const int min_bound_x = std::min(world_bounds.min.world_x(), world_bounds.max.world_x());
    const int max_bound_x = std::max(world_bounds.min.world_x(), world_bounds.max.world_x());
    const int min_bound_z = std::min(world_bounds.min.world_z(), world_bounds.max.world_z());
    const int max_bound_z = std::max(world_bounds.min.world_z(), world_bounds.max.world_z());

    std::vector<world::GridPoint*> query_grid_points;
    std::vector<world::GridPoint*>* grid_points = &query_grid_points;
    const std::vector<world::Chunk*>& active_chunks = world_grid.active_chunks();
    std::uint32_t active_chunk_points_scanned = 0;
    std::uint32_t asset_to_point_lookups_avoided = 0;
    if (!active_chunks.empty()) {
        std::size_t estimated_points = 0;
        for (const world::Chunk* chunk : active_chunks) {
            if (chunk) {
                estimated_points += chunk->resident_point_ids.size();
                asset_to_point_lookups_avoided = static_cast<std::uint32_t>(
                    std::min<std::size_t>(static_cast<std::size_t>(asset_to_point_lookups_avoided) + chunk->assets.size(),
                                          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
            }
        }
        active_chunk_grid_point_scratch_.clear();
        active_chunk_grid_point_scratch_.reserve(estimated_points);
        for (const world::Chunk* chunk : active_chunks) {
            if (!chunk) {
                continue;
            }
            for (world::GridId point_id : chunk->resident_point_ids) {
                world::GridPoint* point = world_grid.point_for_id(point_id);
                if (!point) {
                    continue;
                }
                if (!point || !point->has_assets_or_active_children()) {
                    continue;
                }
                ++active_chunk_points_scanned;
                if (point->resolution_layer() < 0 ||
                    point->resolution_layer() > world_grid.max_resolution_layers()) {
                    continue;
                }
                if (point->world_z() < min_world_z || point->world_z() > max_world_z) {
                    continue;
                }
                if (point->world_x() < min_bound_x || point->world_x() > max_bound_x ||
                    point->world_z() < min_bound_z || point->world_z() > max_bound_z) {
                    continue;
                }
                if (point->occupants.empty()) {
                    continue;
                }
                active_chunk_grid_point_scratch_.push_back(point);
#ifndef NDEBUG
                bool has_live_occupant = false;
                for (const std::unique_ptr<Asset>& occupant : point->occupants) {
                    if (occupant && !occupant->dead) {
                        has_live_occupant = true;
                        break;
                    }
                }
                SDL_assert(has_live_occupant && "Chunk resident point list should only contain points with live occupants.");
#endif
            }
        }
        region_metrics.nodes_visited = active_chunk_points_scanned;
        if (!active_chunk_grid_point_scratch_.empty()) {
            grid_points = &active_chunk_grid_point_scratch_;
        }
    }
    phase_candidate_ms = ms_since(profiler_enabled ? profiler_begin : 0);
    if (grid_points->empty()) {
        query_grid_points = world_grid.query_region(
            world_bounds,
            0,
            world_grid.max_resolution_layers(),
            min_world_z,
            max_world_z,
            /*skip_inactive_branches=*/true,
            /*include_empty_nodes=*/false,
            &region_metrics);
        grid_points = &query_grid_points;
    }
    last_nodes_visited_ = region_metrics.nodes_visited;
    last_branches_skipped_ = region_metrics.branches_skipped;
    last_active_chunk_points_scanned_ = active_chunk_points_scanned;
    last_asset_to_point_lookups_avoided_ = asset_to_point_lookups_avoided;
    warped_points_.reserve(grid_points->size());
    visible_traversal_entries_.reserve(grid_points->size() * 2);

    const double anchor_depth = cam_state.anchor_world_z;
    runtime_camera_height_ = cam_state.camera_height;
    runtime_focus_depth_ = cam_state.focus_depth;
    runtime_anchor_world_z_ = cam_state.anchor_world_z;
    runtime_focus_ndc_offset_ = cam_state.focus_ndc_offset;
    runtime_pitch_rad_ = cam_state.pitch_radians;
    runtime_pitch_deg_ = cam_state.pitch_degrees;
    runtime_depth_offset_px_ = static_cast<float>(cam_state.reference_depth);
    projection_calls_total_ = 0;
    projection_calls_saved_early_ = 0;
    assets_stageA_reject_ = 0;
    assets_stageC_entered_ = 0;
    constexpr std::uint32_t kProjectionBudgetBase = 2048;
    projection_recompute_budget_ = static_cast<std::uint32_t>(std::min<std::size_t>(
        grid_points->size(),
        static_cast<std::size_t>(kProjectionBudgetBase)));
    projection_points_deferred_ = 0;
    projection_points_updated_ = 0;
    last_traversal_complete_ = true;
    last_traversal_projection_budget_exhausted_ = false;
    last_traversal_points_without_projection_ = 0;
    last_traversal_camera_mismatch_deferred_ = 0;
    last_traversal_frame_id_ = frame_id;
    std::uint32_t projection_budget_remaining = projection_recompute_budget_;
    if (!cam_state.valid) {
        last_min_world_z_ = 0;
        last_max_world_z_ = 0;
        last_depth_culled_ = 0;
        rebuild_grid_bounds();
        return;
    }
    const float horizon_band = horizon_fade_for_height(cam_state.camera_height);
    last_min_world_z_ = std::numeric_limits<int>::max();
    last_max_world_z_ = std::numeric_limits<int>::min();
    last_depth_culled_ = 0;

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

    struct ProjectedAssetBounds {
        float min_x = 0.0f;
        float max_x = 0.0f;
        float min_y = 0.0f;
        float max_y = 0.0f;
        float largest_dim_px = 0.0f;
    };
    struct AssetVisibilityResult {
        bool visible = false;
        bool geometry_overlap = false;
        bool tile_overlap = false;
        bool light_overlap = false;
        bool conservative_include = false;
        std::optional<ProjectedAssetBounds> projected_bounds{};
        float max_light_diameter_px = 0.0f;
        std::uint8_t reason_flags = 0;
    };
    constexpr std::uint8_t kVisibilityReasonSprite = 0x1;
    constexpr std::uint8_t kVisibilityReasonTile = 0x2;
    constexpr std::uint8_t kVisibilityReasonLight = 0x4;
    constexpr std::uint8_t kVisibilityReasonConservative = 0x8;

    auto asset_has_runtime_light = [](const Asset* asset) -> bool {
        if (!asset || !asset->current_frame) {
            return false;
        }
        for (const DisplacedAssetAnchorPoint& frame_anchor : asset->current_frame->anchor_points) {
            if (frame_anchor.is_valid() && frame_anchor.has_light_data && !frame_anchor.hidden) {
                return true;
            }
        }
        return false;
    };

    auto evaluate_asset_visibility = [&](Asset* asset,
                                         const world::GridPoint* gp,
                                         double base_world_z) -> AssetVisibilityResult {
        AssetVisibilityResult result{};
        if (!asset || !asset->info) {
            return result;
        }

        bool had_any_projection_sample = false;
        bool had_any_projection_success = false;
        auto try_project = [&](double world_x, double world_y, double world_z, SDL_FPoint& out) -> bool {
            had_any_projection_sample = true;
            ++projection_calls_total_;
            if (!project_screen_point(world_x, world_y, world_z, out)) {
                return false;
            }
            had_any_projection_success = true;
            return true;
        };

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

        if (const auto& tiling = asset->tiling_info(); tiling && tiling->is_valid()) {
            const double left = static_cast<double>(tiling->coverage.x);
            const double right = static_cast<double>(tiling->coverage.x + tiling->coverage.w);
            const double top_z = static_cast<double>(tiling->coverage.y);
            const double bottom_z = static_cast<double>(tiling->coverage.y + tiling->coverage.h);
            SDL_FPoint p{};
            if (try_project(left, 0.0, top_z, p)) expand_bounds(p);
            if (try_project(right, 0.0, top_z, p)) expand_bounds(p);
            if (try_project(left, 0.0, bottom_z, p)) expand_bounds(p);
            if (try_project(right, 0.0, bottom_z, p)) expand_bounds(p);
            if (have_projected_bounds &&
                rect_intersects_screen_bounds(cull_bounds, min_x, min_y, max_x, max_y)) {
                result.tile_overlap = true;
                result.visible = true;
            }
        } else {
            float authored_scale = 1.0f;
            if (std::isfinite(asset->info->scale_factor) && asset->info->scale_factor > 0.0f) {
                authored_scale = asset->info->scale_factor;
            }
            constexpr float kCullBoundsSafetyInflation = 1.15f;
            authored_scale *= kCullBoundsSafetyInflation;

            const float fw = static_cast<float>(std::max(1, asset->info->original_canvas_width));
            const float fh = static_cast<float>(std::max(1, asset->info->original_canvas_height));
            const float width = fw * authored_scale;
            const float height = fh * authored_scale;
            const float half_w = width * 0.5f;

            float center_x = asset->smoothed_translation_x();
            float bottom = asset->smoothed_translation_y();
            if (!std::isfinite(center_x)) {
                center_x = static_cast<float>(asset->world_x());
            }
            if (!std::isfinite(bottom)) {
                bottom = static_cast<float>(asset->world_y());
            }
            const double left = static_cast<double>(center_x - half_w);
            const double right = static_cast<double>(center_x + half_w);
            const double world_height = static_cast<double>(std::max(1.0f, height));
            SDL_FPoint p{};
            if (try_project(left, static_cast<double>(bottom), base_world_z, p)) expand_bounds(p);
            if (try_project(right, static_cast<double>(bottom), base_world_z, p)) expand_bounds(p);
            if (try_project(left, static_cast<double>(bottom), base_world_z + world_height, p)) expand_bounds(p);
            if (try_project(right, static_cast<double>(bottom), base_world_z + world_height, p)) expand_bounds(p);
            if (have_projected_bounds &&
                rect_intersects_screen_bounds(cull_bounds, min_x, min_y, max_x, max_y)) {
                result.geometry_overlap = true;
                result.visible = true;
            }
        }

        if (have_projected_bounds) {
            const float width_px = max_x - min_x;
            const float height_px = max_y - min_y;
            if (std::isfinite(width_px) && std::isfinite(height_px) &&
                width_px > 0.0f && height_px > 0.0f) {
                result.projected_bounds = ProjectedAssetBounds{
                    min_x,
                    max_x,
                    min_y,
                    max_y,
                    std::max(width_px, height_px)};
            }
        }

        if (asset->current_frame && settings_.light_radius_overlap_culling_enabled && !result.visible) {
            for (const DisplacedAssetAnchorPoint& frame_anchor : asset->current_frame->anchor_points) {
                if (!frame_anchor.is_valid() || !frame_anchor.has_light_data || frame_anchor.hidden) {
                    continue;
                }
                std::optional<AnchorPoint> resolved = asset->anchor_state(
                    frame_anchor.name,
                    anchor_points::GridMaterialization::None,
                    Asset::AnchorResolveMode::Cached);
                if (!resolved.has_value() || !resolved->exists) {
                    if (asset->is_anchor_cached_unresolved(frame_anchor.name)) {
                        ++anchor_visibility_debug_counters_.anchor_null_cache_hits;
                        ++anchor_visibility_debug_counters_.anchor_force_recompute_skipped;
                    } else {
                        ++anchor_visibility_debug_counters_.anchor_force_recompute_calls;
                        resolved = asset->anchor_state(
                            frame_anchor.name,
                            anchor_points::GridMaterialization::None,
                            Asset::AnchorResolveMode::ForceRecompute);
                    }
                }
                if (!resolved.has_value() || !resolved->exists) {
                    continue;
                }

                AnchorLightData light = frame_anchor.light;
                light.sanitize();
                const float radius_world = std::max(AnchorLightData::kMinRadius, light.radius);

                SDL_FPoint center_screen = resolved->screen_pos_2d;
                if (!std::isfinite(center_screen.x) || !std::isfinite(center_screen.y)) {
                    if (!try_project(resolved->world_exact_pos_2d.x,
                                     resolved->world_exact_pos_2d.y,
                                     resolved->world_exact_z,
                                     center_screen)) {
                        continue;
                    }
                } else {
                    had_any_projection_success = true;
                }

                SDL_FPoint edge_screen{};
                float radius_px = 0.0f;
                if (try_project(resolved->world_exact_pos_2d.x + radius_world,
                                resolved->world_exact_pos_2d.y,
                                resolved->world_exact_z,
                                edge_screen)) {
                    const float dx = edge_screen.x - center_screen.x;
                    const float dy = edge_screen.y - center_screen.y;
                    radius_px = std::sqrt(dx * dx + dy * dy);
                } else if (resolved->has_flat_perspective_scale &&
                           std::isfinite(resolved->flat_perspective_scale) &&
                           resolved->flat_perspective_scale > 0.0f) {
                    radius_px = radius_world * resolved->flat_perspective_scale;
                }

                if (!std::isfinite(radius_px) || radius_px <= 0.0f) {
                    continue;
                }
                result.max_light_diameter_px = std::max(result.max_light_diameter_px, radius_px * 2.0f);
                if (rect_intersects_screen_bounds(cull_bounds,
                                                  center_screen.x - radius_px,
                                                  center_screen.y - radius_px,
                                                  center_screen.x + radius_px,
                                                  center_screen.y + radius_px)) {
                    result.light_overlap = true;
                }
            }
        }

        if (!result.visible && settings_.light_radius_overlap_culling_enabled && result.light_overlap) {
            result.visible = true;
        }
        if (!result.visible && had_any_projection_sample && !had_any_projection_success) {
            result.conservative_include = true;
            result.visible = true;
        }
        if (result.geometry_overlap) result.reason_flags |= kVisibilityReasonSprite;
        if (result.tile_overlap) result.reason_flags |= kVisibilityReasonTile;
        if (result.light_overlap) result.reason_flags |= kVisibilityReasonLight;
        if (result.conservative_include) result.reason_flags |= kVisibilityReasonConservative;
        (void)gp;
        return result;
    };

    std::vector<Asset*> frustum_hits;
    frustum_hits.reserve(8);

    for (world::GridPoint* gp : *grid_points) {
        if (!gp) continue;

        gp->is_floor = (gp->world_y() == 0);

        if (gp->occupants.empty()) {
            continue;
        }

        const double base_world_z = static_cast<double>(gp->world_z());
        const double point_depth_from_anchor = render_depth::depth_from_anchor(anchor_depth, base_world_z);
        const double point_depth_distance = std::fabs(point_depth_from_anchor);
        bool allow_far_light_pass = false;
        if (std::isfinite(point_depth_distance) &&
            point_depth_distance <= static_cast<double>(settings_.light_max_cull_depth)) {
            for (const auto& owned : gp->occupants) {
                if (asset_has_runtime_light(owned.get())) {
                    allow_far_light_pass = true;
                    break;
                }
            }
        }
        if (!std::isfinite(point_depth_distance) ||
            (point_depth_distance > static_cast<double>(settings_.max_cull_depth) && !allow_far_light_pass)) {
            ++last_depth_culled_;
            continue;
        }

        const auto& projection_cache = gp->projection_cache();
        const bool camera_mismatch = projection_cache.last_camera_state_version != camera_state_version_;
        const bool stale_for_frame = projection_cache.screen_data_frame_updated != frame_stamp;
        bool needs_projection = !projection_cache.screen_data_valid || camera_mismatch || stale_for_frame;

        if (needs_projection) {
            const bool has_cached_screen = projection_cache.screen_data_valid;
            const bool near_visible_region = has_cached_screen &&
                rect_contains_screen_point(cull_bounds, projection_cache.screen.x, projection_cache.screen.y, 64.0f);
            const bool can_defer = has_cached_screen && camera_mismatch && !near_visible_region;
            if (can_defer && projection_budget_remaining == 0) {
                ++projection_points_deferred_;
                last_traversal_projection_budget_exhausted_ = true;
                ++last_traversal_camera_mismatch_deferred_;
                needs_projection = false;
            }
        }

        if (needs_projection) {
            if (projection_budget_remaining == 0) {
                ++projection_points_deferred_;
                last_traversal_projection_budget_exhausted_ = true;
                if (camera_mismatch) {
                    ++last_traversal_camera_mismatch_deferred_;
                }
                if (!projection_cache.screen_data_valid) {
                    ++last_traversal_points_without_projection_;
                    last_traversal_complete_ = false;
                }
                needs_projection = false;
            }
            if (needs_projection) {
                world::CameraProjectionParams params = camera_state_to_projection_params(
                    cam_state, screen_width_, screen_height_, horizon_band);
                params.state_version = camera_state_version_;

                gp->project_to_screen(params);
                gp->mark_screen_data_updated(frame_stamp);
                --projection_budget_remaining;
                ++projection_points_updated_;
            }
        }

        const int z_floor = static_cast<int>(std::floor(base_world_z));
        const int z_ceil  = static_cast<int>(std::ceil(base_world_z));
        last_min_world_z_ = std::min(last_min_world_z_, z_floor);
        last_max_world_z_ = std::max(last_max_world_z_, z_ceil);

        frustum_hits.clear();
        for (const auto& owned : gp->occupants) {
            if (!owned) {
                continue;
            }

            asset_to_point_[owned.get()] = gp;
            const double asset_depth_distance = std::fabs(render_depth::depth_from_anchor(
                anchor_depth,
                static_cast<double>(owned->world_z()),
                owned->render_depth_bias()));
            if (!keep_dynamic_asset_for_visibility(settings_, *owned, asset_depth_distance)) {
                continue;
            }
            const AssetVisibilityResult visibility = evaluate_asset_visibility(owned.get(), gp, base_world_z);
            if (visibility.reason_flags != 0) {
                visibility_reason_flags_[owned.get()] = visibility.reason_flags;
            }
            if (visibility.visible) {
                const bool is_boundary_asset =
                    owned->info &&
                    asset_types::canonicalize(owned->info->type) == std::string(asset_types::boundary);
                const float min_visible_ratio = is_boundary_asset
                    ? settings_.boundary_min_visible_screen_ratio
                    : settings_.min_visible_screen_ratio;
                const float min_visible_px =
                    static_cast<float>(screen_height_) * std::clamp(min_visible_ratio, 0.0f, 0.5f);
                if (owned.get() != tracked_player_asset_ && min_visible_px > 0.0f) {
                    const bool preserve_runtime_lights =
                        (visibility.reason_flags & kVisibilityReasonLight) != 0;
                    if (!preserve_runtime_lights) {
                        float effective_largest_dim = 0.0f;
                        if (visibility.projected_bounds.has_value()) {
                            effective_largest_dim = visibility.projected_bounds->largest_dim_px;
                        }
                        effective_largest_dim = std::max(effective_largest_dim, visibility.max_light_diameter_px);
                        if (effective_largest_dim > 0.0f &&
                            effective_largest_dim < min_visible_px) {
                            continue;
                        }
                    }
                }
                frustum_hits.push_back(owned.get());
            }
        }

        gp->mutable_projection_cache().on_screen = !frustum_hits.empty();

        warped_points_.push_back(gp);
        if (!frustum_hits.empty()) {
            for (Asset* visible_asset : frustum_hits) {
                if (!visible_asset) {
                    continue;
                }
                const double depth_from_anchor = render_depth::depth_from_anchor(
                    anchor_depth,
                    static_cast<double>(visible_asset->world_z()),
                    visible_asset->render_depth_bias());
                const double depth_distance = std::fabs(depth_from_anchor);
                visible_traversal_entries_.push_back(VisibleTraversalEntry{
                    visible_asset,
                    gp,
                    depth_from_anchor});
                const float effective_depth_limit =
                    asset_has_runtime_light(visible_asset)
                        ? settings_.light_max_cull_depth
                        : settings_.max_cull_depth;
                if (!std::isfinite(visible_traversal_entries_.back().depth_from_anchor) ||
                    depth_distance > static_cast<double>(effective_depth_limit)) {
                    visible_traversal_entries_.pop_back();
                    ++last_depth_culled_;
                }
            }
        }
    }

    if (!visible_traversal_entries_.empty()) {
        std::stable_sort(
            visible_traversal_entries_.begin(),
            visible_traversal_entries_.end(),
            [](const VisibleTraversalEntry& lhs, const VisibleTraversalEntry& rhs) {
                return lhs.depth_from_anchor > rhs.depth_from_anchor;
            });
    }

    if (last_min_world_z_ == std::numeric_limits<int>::max()) {
        last_min_world_z_ = 0;
        last_max_world_z_ = 0;
    }

    if (profiler_enabled) {
        const double total_ms = ms_since(profiler_begin);
        auto& rc = rolling_costs();
        rc.values.push_back(total_ms);
        if (rc.values.size() > 240) rc.values.pop_front();
        const double p50 = percentile(rc.values, 50.0);
        const double p95 = percentile(rc.values, 95.0);
        const double p99 = percentile(rc.values, 99.0);
        phase_traversal_ms = total_ms - (phase_view_ms + phase_candidate_ms + phase_projection_ms + phase_visibility_ms);
        std::ostringstream ss;
        ss << "grid:view_ms=" << phase_view_ms
           << ",candidate_ms=" << phase_candidate_ms
           << ",projection_ms=" << phase_projection_ms
           << ",visibility_ms=" << phase_visibility_ms
           << ",traversal_ms=" << phase_traversal_ms
           << ",points_scanned=" << grid_points->size()
           << ",assets_scanned=" << assets_stageC_entered_
           << ",assets_projected=" << projection_calls_total_
           << ",anchors_recomputed=" << anchor_visibility_debug_counters_.anchor_force_recompute_calls
           << ",frame_ms=" << total_ms << ",p50=" << p50 << ",p95=" << p95 << ",p99=" << p99;
        render_diagnostics::set_render_stage_timings(ss.str());
        if (total_ms > render_profiler_spike_ms()) {
            vibble::log::warn("[WarpedScreenGridProfiler] spike frame_ms=" + std::to_string(total_ms) +
                              " projection_calls=" + std::to_string(projection_calls_total_) +
                              " assets_stageC=" + std::to_string(assets_stageC_entered_) +
                              " points=" + std::to_string(grid_points->size()));
        }
    }
    render_diagnostics::set_visibility_projection_stats(projection_calls_total_,
                                                     projection_calls_saved_early_,
                                                     assets_stageA_reject_,
                                                     assets_stageC_entered_,
                                                     projection_recompute_budget_,
                                                     projection_points_deferred_,
                                                     projection_points_updated_);

    rebuild_grid_bounds();
    bounds_.left   = cull_bounds.left;
    bounds_.top    = cull_bounds.top;
    bounds_.right  = cull_bounds.right;
    bounds_.bottom = cull_bounds.bottom;
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
    for (world::GridPoint* gp : warped_points_) {
        if (!gp) continue;
        if (!gp->is_on_screen()) {
            continue;
        }
        const SDL_FPoint screen = gp->screen_position();
        const float dx = screen.x - static_cast<float>(screen_pt.x);
        const float dy = screen.y - static_cast<float>(screen_pt.y);
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
    invalidate_camera_cache();
}

void WarpedScreenGrid::clear_focus_override() {
    camera_.clear_focus_override();
    invalidate_camera_cache();
}





double WarpedScreenGrid::default_camera_height_for_room(const Room* room) const {
    return std::max(1.0, compute_room_scale_from_area(room));
}

void WarpedScreenGrid::project_to_screen(world::GridPoint& point) const {
    const CameraState& cam_state = camera_state_cached();
    const float horizon_band = horizon_fade_for_height(cam_state.camera_height);

    // השתמש בשיטת ההקרנה העצמאית של GridPoint
    world::CameraProjectionParams params = camera_state_to_projection_params(
        cam_state, screen_width_, screen_height_, horizon_band);
    params.state_version = camera_state_version_;

    point.project_to_screen(params);

    // הערה: GridPoint משתמש בקואורדינטות מסך שבהן Y גדל כלפי מטה,
    // אך ההקרנה הפנימית משתמשת ב-Y שגדל כלפי מעלה. שיטת GridPoint
    // מטפלת בכך פנימית, ולכן אין צורך בהיפוך Y כאן.
}

bool WarpedScreenGrid::is_manual_height_override() const {
    return camera_.manual_height_override();
}

void WarpedScreenGrid::set_manual_height_override(bool v) {
    camera_.set_manual_height_override(v);
    invalidate_camera_cache();
}

bool WarpedScreenGrid::is_manual_zoom_override() const {
    return camera_.manual_zoom_override();
}

void WarpedScreenGrid::set_manual_zoom_override(bool v) {
    camera_.set_manual_zoom_override(v);
    invalidate_camera_cache();
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
    invalidate_camera_cache();
}

double WarpedScreenGrid::get_zoom_percent() const {
    return camera_.state().params.zoom_percent;
}

void WarpedScreenGrid::set_zoom_percent(double percent) {
    camera_.set_zoom_percent(percent);
    invalidate_camera_cache();
}

void WarpedScreenGrid::adjust_zoom_percent(double delta_percent) {
    const double current = std::clamp(camera_.state().params.zoom_percent, 0.0, 100.0);
    const double target = std::clamp(current + delta_percent, 0.0, 100.0);
    camera_.set_zoom_percent(target);
    invalidate_camera_cache();
}

void WarpedScreenGrid::set_tilt_override(std::optional<float> tilt_deg) {
    if (!tilt_deg.has_value() || !std::isfinite(*tilt_deg)) {
        tilt_override_deg_.reset();
        invalidate_camera_cache();
        return;
    }
    tilt_override_deg_ = camera_math::sanitize_pitch_degrees(*tilt_deg);
    invalidate_camera_cache();
}

void WarpedScreenGrid::clear_tilt_override() {
    tilt_override_deg_.reset();
    invalidate_camera_cache();
}

std::optional<float> WarpedScreenGrid::tilt_override() const {
    return tilt_override_deg_;
}

void WarpedScreenGrid::update() {
    camera_.tick(0.0f);
    const CameraState& cam = camera_state_cached();
    runtime_camera_height_ = cam.camera_height;
    runtime_pitch_deg_ = cam.pitch_degrees;
    runtime_pitch_rad_ = cam.pitch_radians;
    runtime_anchor_world_z_ = cam.anchor_world_z;
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
    // היסט pan כ-offset בציר x, וגובה כ-offset בציר y מהמרכז
    SDL_Point center = get_screen_center();
    return {center.x + static_cast<int>(pan), center.y + static_cast<int>(height)};
}

void WarpedScreenGrid::animate_height_multiply(double factor) {
    camera_.animate_height_multiply(factor);
    invalidate_camera_cache();
}

void WarpedScreenGrid::animate_height_towards_point(double target_height, SDL_Point target_point) {
    camera_.animate_height_towards_point(target_height, target_point);
    invalidate_camera_cache();
}

bool WarpedScreenGrid::is_height_animating() const {
    return camera_.is_animating();
}

SDL_Point WarpedScreenGrid::pan_and_height_to_asset(double pan, double height, const Asset* asset) const {
    if (!asset) return {0, 0};
    // התאם את מיקום הנכס לפי pan וגובה
    return {asset->world_x() + static_cast<int>(pan), asset->world_y() + static_cast<int>(height)};
}
