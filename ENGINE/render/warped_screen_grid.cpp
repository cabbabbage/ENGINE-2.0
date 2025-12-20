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
    constexpr float  kMinTau    = 1e-4f;
    constexpr double SCALE_EPS  = 1e-4;
    constexpr double BASE_RATIO = 1.1;
    constexpr double PI_D       = 3.14159265358979323846;
    constexpr double kHalfFovY  = PI_D / 4.0;
    constexpr double kBottomAngleLimit = (PI_D * 0.5) - 1e-3;
    constexpr float  kDefaultPitchDegrees   = 60.0f;
    constexpr double kMinHeightRange = 1e-4;
    constexpr double kMinPerspectiveScale   = 0.35;
    constexpr double kMaxPerspectiveScale   = 1.65;
    struct HeightInterpolator {
        double t = 0.0;
        HeightInterpolator(const WarpedScreenGrid::RealismSettings& settings, double scale_value) {
            const double safe_low = std::max(static_cast<double>(WarpedScreenGrid::kMinHeightAnchors), static_cast<double>(settings.camera_height_min));
            const double safe_high = std::max(safe_low + kMinHeightRange, static_cast<double>(settings.camera_height_max));
            const double span = std::max(kMinHeightRange, safe_high - safe_low);
            t = std::clamp((scale_value - safe_low) / span, 0.0, 1.0);
        }

        template <typename V>
        V lerp(V low, V high) const {
            return ::lerp(low, high, t);
        }
};

    double wrap_degrees_0_360(double raw_value) {
        if (!std::isfinite(raw_value)) {
            return static_cast<double>(kDefaultPitchDegrees);
        }
        double wrapped = std::fmod(raw_value, 360.0);
        if (wrapped < 0.0) wrapped += 360.0;
        if (wrapped >= 360.0 || !std::isfinite(wrapped)) {
            wrapped = std::fmod(wrapped, 360.0);
            if (wrapped < 0.0) wrapped += 360.0;
        }
        return std::isfinite(wrapped) ? wrapped : static_cast<double>(kDefaultPitchDegrees);
    }

    float wrap_degrees_0_360(float raw_value) {
        return static_cast<float>(wrap_degrees_0_360(static_cast<double>(raw_value)));
    }

    double shortest_delta_degrees(double from_deg, double to_deg);

    double lerp_angle(double from_deg, double to_deg, double t) {
        const double delta = shortest_delta_degrees(from_deg, to_deg);
        return wrap_degrees_0_360(from_deg + delta * t);
    }

    double signed_radians_from_degrees(double degrees) {
        const double wrapped_deg = wrap_degrees_0_360(degrees);
        const double signed_deg  = (wrapped_deg > 180.0) ? wrapped_deg - 360.0 : wrapped_deg;
        return signed_deg * (PI_D / 180.0);
    }

    double shortest_delta_degrees(double from_deg, double to_deg) {
        return std::remainder(to_deg - from_deg, 360.0);
    }

    float sanitize_pitch_degrees(float raw_value, bool* clamped = nullptr) {
        if (clamped) *clamped = false;
        const float wrapped = wrap_degrees_0_360(std::isfinite(raw_value) ? raw_value : kDefaultPitchDegrees);
        const float clamped_value = std::clamp( wrapped, WarpedScreenGrid::kMinPitchDegrees, WarpedScreenGrid::kMaxPitchDegrees);
        if (clamped && clamped_value != raw_value) {
            *clamped = true;
        }
        return clamped_value;
    }

    TransformSmoothingParams sanitize_params(const TransformSmoothingParams& params) {
        TransformSmoothingParams out = params;
        if (!std::isfinite(out.lerp_rate) || out.lerp_rate < 0.0f) {
            out.lerp_rate = 0.0f;
        }
        if (!std::isfinite(out.spring_frequency) || out.spring_frequency < 0.0f) {
            out.spring_frequency = 0.0f;
        }
        if (!std::isfinite(out.max_step) || out.max_step < 0.0f) {
            out.max_step = 0.0f;
        }
        if (!std::isfinite(out.snap_threshold) || out.snap_threshold < 0.0f) {
            out.snap_threshold = 0.0f;
        }
        switch (out.method) {
        case TransformSmoothingMethod::None:
        case TransformSmoothingMethod::Lerp:
        case TransformSmoothingMethod::CriticallyDampedSpring:
            break;
        default:
            out.method = TransformSmoothingMethod::None;
            break;
        }
        return out;
    }

    float rate_from_tau(float tau_seconds) {
        if (!std::isfinite(tau_seconds) || tau_seconds <= kMinTau) {
            return 0.0f;
        }
        return 1.0f / tau_seconds;
    }

    float tau_from_rate(float rate) {
        if (!std::isfinite(rate) || rate <= kMinTau) {
            return 0.0f;
        }
        return 1.0f / rate;
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

    double clamp_height_scale(double value) {
        return std::clamp( value, 0.0001, static_cast<double>(WarpedScreenGrid::kMaxHeightAnchors));
    }

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
        double camera_height = 0.0;
        double focus_depth   = 0.0;
        double reference_depth = 1.0;
        double tan_half_fov_y = std::tan(kHalfFovY);
        double tan_half_fov_x = std::tan(kHalfFovY);
        double near_plane = 0.0;
        double far_plane  = 0.0;
        double horizon_screen_y = 0.0;
    };

    CameraState build_camera_state(const WarpedScreenGrid::RealismSettings& settings,
                                   double aspect,
                                   int screen_width,
                                   int screen_height,
                                   SDL_FPoint anchor_world,
                                   double scale_value) {
        CameraState state{};

        const double clamped_scale = std::max(0.0001, scale_value);
        const double base_height   = std::max(1.0, static_cast<double>(settings.base_height_px));
        const double camera_height = base_height * clamped_scale;
        if (!std::isfinite(camera_height) || camera_height <= 0.0) {
            return state;
        }

        const double pitch_deg = sanitize_pitch_degrees(kDefaultPitchDegrees);
        const double pitch_rad = signed_radians_from_degrees(pitch_deg);
        const double tan_pitch = std::tan(pitch_rad);
        if (!std::isfinite(tan_pitch) || std::abs(tan_pitch) < 1e-6) {
            return state;
        }

        const double focus_depth = camera_height / tan_pitch;
        const Vec3   camera_pos{
            static_cast<double>(anchor_world.x),
            static_cast<double>(anchor_world.y) - focus_depth,
            camera_height
        };
        const Vec3 target{
            static_cast<double>(anchor_world.x),
            static_cast<double>(anchor_world.y),
            0.0
        };
        Vec3 forward = normalize(target - camera_pos);
        const Vec3 up_world{0.0, 0.0, 1.0};
        Vec3 right = normalize(cross(forward, up_world));
        if (length(right) < 1e-6) {
            right = Vec3{1.0, 0.0, 0.0};
        }
        Vec3 up = normalize(cross(right, forward));
        const double ref_depth = length(target - camera_pos);

        const double tan_half_fov_y = std::tan(kHalfFovY);
        const double tan_half_fov_x = tan_half_fov_y * std::max(1e-6, aspect);

        const double tan_fov = tan_half_fov_y;
        const double horizon_ndc = tan_pitch / tan_fov;
        const double screen_h    = std::max(1.0, static_cast<double>(screen_height));
        const double horizon_y   = screen_h * (0.5 - 0.5 * horizon_ndc);

        state.valid           = std::isfinite(ref_depth) && ref_depth > 0.0;
        state.position        = camera_pos;
        state.forward         = forward;
        state.right           = right;
        state.up              = up;
        state.camera_height   = camera_height;
        state.focus_depth     = focus_depth;
        state.reference_depth = ref_depth;
        state.tan_half_fov_y  = tan_half_fov_y;
        state.tan_half_fov_x  = tan_half_fov_x;
        state.near_plane      = std::max(0.1, static_cast<double>(settings.depth_near_world));
        state.far_plane       = std::max(state.near_plane + 1.0, static_cast<double>(settings.depth_far_world));
        state.horizon_screen_y = horizon_y;
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

    ProjectionResult project_world_point(const CameraState& cam,
                                         double world_x,
                                         double world_y,
                                         double world_z,
                                         int screen_width,
                                         int screen_height,
                                         float horizon_band_px) {
        ProjectionResult out{};
        if (!cam.valid) {
            return out;
        }

        const Vec3 world{
            static_cast<double>(world_x),
            static_cast<double>(world_y),
            static_cast<double>(world_z)
        };
        const Vec3 to_point = world - cam.position;
        const double depth_along_forward = dot(to_point, cam.forward);
        const double distance_to_camera  = length(to_point);
        if (depth_along_forward <= cam.near_plane ||
            distance_to_camera < cam.near_plane ||
            distance_to_camera > cam.far_plane ||
            !std::isfinite(distance_to_camera)) {
            return out;
        }

        const double cam_x = dot(to_point, cam.right);
        const double cam_y = dot(to_point, cam.up);

        const double ndc_x = (cam_x / depth_along_forward) / cam.tan_half_fov_x;
        const double ndc_y = (cam_y / depth_along_forward) / cam.tan_half_fov_y;

        const double screen_x = (ndc_x * 0.5 + 0.5) * static_cast<double>(screen_width);
        const double screen_y = (0.5 - ndc_y * 0.5) * static_cast<double>(screen_height);

        const float perspective = static_cast<float>(cam.reference_depth / std::max(depth_along_forward, 1e-4));
        const float vertical    = perspective;

        float horizon_fade = 1.0f;
        if (horizon_band_px > 0.0f) {
            const float dist_from_horizon = static_cast<float>(screen_y - cam.horizon_screen_y);
            if (dist_from_horizon <= 0.0f) {
                horizon_fade = 0.0f;
            } else if (dist_from_horizon < horizon_band_px) {
                const float t = dist_from_horizon / horizon_band_px;
                horizon_fade = std::clamp(t * t * t, 0.0f, 1.0f);
            }
        }

        out.valid             = std::isfinite(screen_x) && std::isfinite(screen_y);
        out.screen            = SDL_FPoint{ static_cast<float>(screen_x), static_cast<float>(screen_y) };
        out.perspective_scale = std::isfinite(perspective) ? perspective : 1.0f;
        out.vertical_scale    = std::isfinite(vertical) ? vertical : 1.0f;
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
        if (std::abs(dir.z) < 1e-6) {
            return std::nullopt;
        }
        const double t = -cam.position.z / dir.z;
        if (!std::isfinite(t) || t <= 0.0) {
            return std::nullopt;
        }
        const Vec3 hit = cam.position + dir * t;
        return SDL_FPoint{ static_cast<float>(hit.x), static_cast<float>(hit.y) };
    }

    double camera_height_from_scale(const WarpedScreenGrid::RealismSettings& settings, double scale_value) {
        const double base_height = std::max(1.0, static_cast<double>(settings.base_height_px));
        return std::max(0.0, base_height * scale_value);
    }

    double solve_pitch_for_fixed_depth(double camera_height,
                                       double desired_depth_world,
                                       double default_pitch_rad) {
        if (!std::isfinite(camera_height) || camera_height <= 0.0) {
            return default_pitch_rad;
        }
        if (!std::isfinite(desired_depth_world) || desired_depth_world <= 0.0) {
            return default_pitch_rad;
        }

        const double min_pitch_rad = std::max(1e-4, static_cast<double>(WarpedScreenGrid::kMinPitchDegrees) * (PI_D / 180.0));
        const double max_pitch_rad = std::min( static_cast<double>(WarpedScreenGrid::kMaxPitchDegrees) * (PI_D / 180.0), kBottomAngleLimit - 1e-4);

        double low = min_pitch_rad;
        double high = std::max(low + 1e-4, max_pitch_rad);

        const auto depth_span = [&](double pitch) -> double {
            const double clamped_pitch = std::clamp(pitch, min_pitch_rad, high);
            const double tan_center = std::tan(clamped_pitch);
            if (!std::isfinite(tan_center) || std::abs(tan_center) < 1e-6) {
                return std::numeric_limits<double>::infinity();
            }
            const double center_depth = camera_height / tan_center;

            const double bottom_angle = std::min(kBottomAngleLimit, clamped_pitch + kHalfFovY);
            const double tan_bottom = std::tan(bottom_angle);
            if (!std::isfinite(tan_bottom) || std::abs(tan_bottom) < 1e-6) {
                return std::numeric_limits<double>::infinity();
            }
            const double bottom_depth = camera_height / tan_bottom;
            return center_depth - bottom_depth;
};

        const double desired = std::max(0.0, desired_depth_world);
        double span_low = depth_span(low);
        double span_high = depth_span(high);
        if (!std::isfinite(span_low) || !std::isfinite(span_high)) {
            return std::clamp(default_pitch_rad, low, high);
        }

        if (desired >= span_low) {
            return low;
        }
        if (desired <= span_high) {
            return high;
        }

        for (int i = 0; i < 48; ++i) {
            const double mid = 0.5 * (low + high);
            const double span_mid = depth_span(mid);
            if (!std::isfinite(span_mid)) {
                high = mid;
                continue;
            }
            if (span_mid > desired) {
                low = mid;
            } else {
                high = mid;
            }
        }

        return std::clamp(high, low, max_pitch_rad);
    }

    WarpedScreenGrid::CameraGeometry build_geometry(const WarpedScreenGrid::RealismSettings& settings,
                                               double scale_value,
                                               double anchor_world_y,
                                               double desired_depth_world,
                                               bool realism_enabled) {
        WarpedScreenGrid::CameraGeometry g{};
        if (!realism_enabled) {
            return g;
        }

        const double clamped_scale = std::max(0.0001, scale_value);
        g.camera_height = camera_height_from_scale(settings, clamped_scale);
        if (g.camera_height <= 0.0) {
            return g;
        }

        const double default_pitch_deg = kDefaultPitchDegrees;
        const double default_pitch_rad = signed_radians_from_degrees(default_pitch_deg);
        const double solved_pitch_rad = solve_pitch_for_fixed_depth( g.camera_height, desired_depth_world, default_pitch_rad);

        const double solved_pitch_deg = solved_pitch_rad * (180.0 / PI_D);
        const float sanitized_deg = sanitize_pitch_degrees(static_cast<float>(solved_pitch_deg));
        g.pitch_degrees = sanitized_deg;
        g.pitch_radians = signed_radians_from_degrees(static_cast<double>(sanitized_deg));

        const double tan_pitch = std::tan(g.pitch_radians);
        if (!std::isfinite(tan_pitch) || std::abs(tan_pitch) < 1e-6) {
            return g;
        }

        g.anchor_world_y = anchor_world_y;
        if (!std::isfinite(g.anchor_world_y)) {
            return g;
        }

        g.focus_depth   = g.camera_height / tan_pitch;
        g.camera_world_y = g.anchor_world_y - g.focus_depth;
        g.focus_ndc_offset = 0.0;

        g.valid = std::isfinite(g.camera_world_y) && std::isfinite(g.focus_depth);
        return g;
    }

    WarpedScreenGrid::FloorDepthParams build_floor_params(
        const WarpedScreenGrid::RealismSettings& settings,
        int screen_height,
        const WarpedScreenGrid::CameraGeometry& geom,
        double scale_value,
        bool realism_enabled) {
        WarpedScreenGrid::FloorDepthParams p{};
        (void)scale_value;
        if (!realism_enabled || !geom.valid) {
            return p;
        }

        const double screen_h = std::max(1.0, static_cast<double>(screen_height));
        if (!std::isfinite(geom.camera_height) ||
            !std::isfinite(geom.pitch_radians) ||
            !std::isfinite(geom.camera_world_y) ||
            !std::isfinite(geom.anchor_world_y)) {
            return p;
        }

        constexpr double kMaxHorizonRatio = 0.45;
        const double max_horizon = screen_h * kMaxHorizonRatio;
        const double min_horizon = -screen_h * 4.0;

        const double tan_fov   = std::tan(kHalfFovY);
        const double tan_pitch = std::tan(geom.pitch_radians);
        if (!std::isfinite(tan_fov) || !std::isfinite(tan_pitch) || std::abs(tan_fov) < 1e-6) {
            return p;
        }

        const double max_phi = (PI_D * 0.5) - 1e-3;
        double phi_bottom = geom.pitch_radians + kHalfFovY;
        phi_bottom = std::clamp(phi_bottom, 1e-3, max_phi);

        const double ndc_bottom_raw = std::tan(geom.pitch_radians - phi_bottom) / tan_fov;
        const double ndc_scale = (std::isfinite(ndc_bottom_raw) && ndc_bottom_raw < -1e-4) ? (-1.0 / ndc_bottom_raw) : 1.0;
        double near_ndc = ndc_bottom_raw * ndc_scale;
        if (!std::isfinite(near_ndc)) {
            near_ndc = -1.0;
        }

        const double horizon_ndc_raw = tan_pitch / tan_fov;
        if (!std::isfinite(horizon_ndc_raw)) {
            return p;
        }
        const double horizon_ndc = horizon_ndc_raw * ndc_scale;
        double horizon_y = screen_h * (0.5 - 0.5 * horizon_ndc);
        horizon_y = std::clamp(horizon_y, min_horizon, max_horizon);

        double pitch_norm = geom.pitch_radians / (kHalfFovY * 2.0);
        pitch_norm = std::clamp(pitch_norm, 0.0, 1.0);

        p.horizon_screen_y = horizon_y;
        p.bottom_screen_y  = screen_h;
        p.base_world_y     = geom.anchor_world_y;
        p.camera_world_y   = geom.camera_world_y;
        p.camera_height    = geom.camera_height;
        p.pitch_radians    = geom.pitch_radians;
        p.pitch_norm       = pitch_norm;
        p.focus_ndc_offset = 0.0;
        p.horizon_ndc      = horizon_ndc;
        p.near_ndc         = near_ndc;
        p.ndc_scale        = ndc_scale;
        p.strength         = 6.0;
        p.enabled          = true;
        (void)settings;
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

    struct PerspectiveRange {
        double near_distance = 0.0;
        double far_distance  = 1.0;
};

    PerspectiveRange sanitize_perspective_range(const WarpedScreenGrid::RealismSettings& settings) {
        double near_distance = static_cast<double>(settings.perspective_distance_at_scale_hundred);
        double far_distance  = static_cast<double>(settings.perspective_distance_at_scale_zero);
        if (!std::isfinite(near_distance)) near_distance = 0.0;
        if (!std::isfinite(far_distance))  far_distance  = near_distance + 1.0;
        if (std::fabs(far_distance - near_distance) < 1e-4) {
            far_distance = near_distance + 1.0;
        }
        if (near_distance > far_distance) {
            std::swap(near_distance, far_distance);
        }
        return PerspectiveRange{ near_distance, far_distance };
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

    double calculate_reference_perspective_scale(
        double screen_y,
        const WarpedScreenGrid::FloorDepthParams& params,
        const PerspectiveRange& range,
        double zoom_factor)
    {

        const double min_y = std::min(params.horizon_screen_y, params.bottom_screen_y);
        const double max_y = std::max(params.horizon_screen_y, params.bottom_screen_y);
        const double denom = std::max(max_y - min_y, 1e-4);
        double t = std::clamp((screen_y - min_y) / denom, 0.0, 1.0);

        float pitch_deg = static_cast<float>(params.pitch_radians * (180.0 / PI_D));
        if (!std::isfinite(pitch_deg)) pitch_deg = kDefaultPitchDegrees;
        pitch_deg = std::clamp(pitch_deg, WarpedScreenGrid::kMinPitchDegrees, WarpedScreenGrid::kMaxPitchDegrees);

        const float min_falloff = 1.0f;
        const float max_falloff = 1.3f;
        float pitch_norm = (pitch_deg - WarpedScreenGrid::kMinPitchDegrees) / (WarpedScreenGrid::kMaxPitchDegrees - WarpedScreenGrid::kMinPitchDegrees);
        float falloff = max_falloff - (max_falloff - min_falloff) * pitch_norm;

        const double s0 = 0.0;
        const double s1 = 1.0;
        const double s2 = 0.7;

        const double a = -4.0 * (s1 - s0 - 0.5 * (s2 - s0));
        const double b = (s2 - s0) - a;
        const double c = s0;

        double smooth_t = t * t * (3.0 - 2.0 * t);

        double regressed = a * smooth_t * smooth_t + b * smooth_t + c;
        regressed = std::clamp(regressed, 0.0, 2.0);
        regressed = std::pow(regressed, falloff);

        const double zoom_reduction = 1.0 - (zoom_factor * 0.3);

        double final_scale = regressed * zoom_reduction;

        return std::clamp(final_scale, 0.5, 2.0);
    }

    double interpolate_perspective_scale(double screen_y, double horizon_y, double bottom_y,
                                         double horizon_scale, double bottom_scale) {

        screen_y = std::clamp(screen_y, horizon_y, bottom_y);

        const double range = std::max(1.0, bottom_y - horizon_y);
        double t = (screen_y - horizon_y) / range;
        t = std::clamp(t, 0.6, 2.0);

        double smooth_t;
        if (t < 0.5) {
            smooth_t = 1.0 * t * t;
        } else {
            smooth_t = 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
        }

        return horizon_scale + (bottom_scale - horizon_scale) * smooth_t;
    }

}

WarpedScreenGrid::CameraGeometry WarpedScreenGrid::compute_geometry_for_scale(double scale_value) const {
    CameraGeometry g{};
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, scale_value);
    if (!cam.valid) {
        return g;
    }

    const double pitch_rad = std::atan2(cam.camera_height, cam.focus_depth);
    g.valid          = true;
    g.camera_height  = cam.camera_height;
    g.focus_depth    = cam.focus_depth;
    g.anchor_world_y = anchor_world_y();
    g.focus_ndc_offset = 0.0;
    g.pitch_radians  = pitch_rad;
    g.pitch_degrees  = static_cast<float>(pitch_rad * (180.0 / PI_D));
    g.camera_world_y = cam.position.y;
    return g;
}

WarpedScreenGrid::CameraGeometry WarpedScreenGrid::compute_geometry() const {
    return compute_geometry_for_scale(static_cast<double>(smoothed_scale_));
}

void WarpedScreenGrid::update_geometry_cache(const CameraGeometry& g) {
    const double scale_value = std::max(0.0001, static_cast<double>(smoothed_scale_));
    runtime_camera_height_   = g.camera_height;
    runtime_focus_depth_     = g.focus_depth;
    runtime_anchor_world_y_  = g.anchor_world_y;
    runtime_focus_ndc_offset_ = g.focus_ndc_offset;
    runtime_pitch_rad_       = g.pitch_radians;
    runtime_pitch_deg_       = g.pitch_degrees;
    runtime_depth_offset_px_ = depth_offset_for_scale(scale_value);

    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, scale_value);
    runtime_floor_params_ = FloorDepthParams{};
    runtime_floor_params_.enabled = cam.valid;
    runtime_floor_params_.camera_height = cam.camera_height;
    runtime_floor_params_.focus_depth   = cam.focus_depth;
    runtime_floor_params_.horizon_screen_y = cam.horizon_screen_y;
    runtime_floor_params_.bottom_screen_y  = static_cast<double>(screen_height_);
    runtime_floor_params_.near_ndc = -1.0;
    runtime_floor_params_.ndc_scale = 1.0;
    runtime_floor_params_.camera_world_y = cam.position.y;

    geometry_valid_ = g.valid && cam.valid;
    if (!geometry_valid_) {
        runtime_camera_height_   = 0.0;
        runtime_focus_depth_     = 0.0;
        runtime_anchor_world_y_  = 0.0;
        runtime_focus_ndc_offset_= 0.0;
        runtime_pitch_rad_       = 0.0;
        runtime_pitch_deg_       = 0.0f;
        runtime_depth_offset_px_ = depth_offset_for_scale(scale_value);
        runtime_floor_params_    = FloorDepthParams{};
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
    screen_center_ = start_center;
    screen_center_initialized_ = true;
    pan_offset_x_ = 0.0;
    pan_offset_y_ = 0.0;

    const int base_w = width_from_area(base_view_);
    const int curr_w = width_from_area(current_view_);
    scale_ = (base_w > 0) ? static_cast<float>(static_cast<double>(curr_w) / static_cast<double>(base_w)) : 1.0f;

    height_animating_     = false;
    steps_total_ = 0;
    steps_done_  = 0;
    start_scale_ = scale_;
    target_scale_ = scale_;

    smoothed_center_.x = static_cast<float>(screen_center_.x);
    smoothed_center_.y = static_cast<float>(screen_center_.y);
    smoothed_scale_    = std::max(0.0001f, scale_);
    update_geometry_cache(compute_geometry());
    realism_enabled_ = true;
    depth_enabled_   = true;
}

WarpedScreenGrid::~WarpedScreenGrid() = default;

void WarpedScreenGrid::set_realism_settings(const RealismSettings& settings) {
    settings_ = settings;
    settings_.zoom_low = std::clamp(settings_.zoom_low, WarpedScreenGrid::kMinZoomAnchors, WarpedScreenGrid::kMaxZoomAnchors);
    const float min_high = std::min(WarpedScreenGrid::kMaxZoomAnchors, settings_.zoom_low + 0.0001f);
    settings_.zoom_high = std::clamp(settings_.zoom_high, min_high, WarpedScreenGrid::kMaxZoomAnchors);
    if (!std::isfinite(settings_.base_height_px) || settings_.base_height_px <= 0.0f) {
        settings_.base_height_px = 720.0f;
    }
    settings_.parallax_smoothing = sanitize_params(settings_.parallax_smoothing);
    if (settings_.parallax_smoothing.method == TransformSmoothingMethod::Lerp &&
        settings_.parallax_smoothing.lerp_rate <= 0.0f) {
        settings_.parallax_smoothing.lerp_rate = rate_from_tau(0.08f);
    } else if (settings_.parallax_smoothing.method == TransformSmoothingMethod::CriticallyDampedSpring &&
               settings_.parallax_smoothing.spring_frequency <= 0.0f) {
        settings_.parallax_smoothing.spring_frequency = 10.0f;
    }

    update_geometry_cache(compute_geometry());
}

void WarpedScreenGrid::set_screen_center(SDL_Point p, bool snap_immediately) {
    if (!screen_center_initialized_) {
        screen_center_              = p;
        screen_center_initialized_  = true;
        pan_offset_x_               = 0.0;
        pan_offset_y_               = 0.0;
        smoothed_center_.x          = static_cast<float>(screen_center_.x);
        smoothed_center_.y          = static_cast<float>(screen_center_.y);
        return;
    }

    const double dx = static_cast<double>(p.x) - static_cast<double>(screen_center_.x);
    const double dy = static_cast<double>(p.y) - static_cast<double>(screen_center_.y);
    pan_offset_x_ += dx;
    pan_offset_y_ += dy;
    screen_center_ = p;
    if (snap_immediately) {
        smoothed_center_.x = static_cast<float>(screen_center_.x);
        smoothed_center_.y = static_cast<float>(screen_center_.y);
    }
}

void WarpedScreenGrid::set_scale(float s) {
    const double clamped = clamp_zoom_scale(static_cast<double>(s));
    scale_ = static_cast<float>(clamped);
    zooming_     = false;
    steps_total_ = 0;
    steps_done_  = 0;
    start_scale_ = scale_;
    target_scale_= scale_;
    smoothed_scale_ = scale_;
    update_geometry_cache(compute_geometry());
}

float WarpedScreenGrid::get_scale() const {
    return smoothed_scale_;
}

void WarpedScreenGrid::zoom_to_scale(double target_scale, int duration_steps) {
    double clamped = clamp_zoom_scale(target_scale);
    if (duration_steps <= 0) {
        set_scale(static_cast<float>(clamped));
        return;
    }
    duration_steps = std::max(1, duration_steps);

    const bool currently_zooming = zooming_ && steps_total_ > 0;
    bool restart_zoom = !currently_zooming || steps_total_ != duration_steps;

    if (!restart_zoom && std::fabs(clamped - target_scale_) > SCALE_EPS) {
        restart_zoom = true;
    }

    if (restart_zoom) {
        start_scale_ = scale_;
        steps_total_ = duration_steps;
        steps_done_  = 0;
    }

    target_scale_ = clamped;
    zooming_      = true;
}

void WarpedScreenGrid::zoom_to_area(const Area& target_area, int duration_steps) {
    Area adjusted = convert_area_to_aspect(target_area);
    const int base_w = std::max(1, width_from_area(base_zoom_));
    const int tgt_w  = std::max(1, width_from_area(adjusted));
    const double target = static_cast<double>(tgt_w) / static_cast<double>(base_w);
    zoom_to_scale(target, duration_steps);
}

void WarpedScreenGrid::update(float dt) {
    if (!std::isfinite(dt) || dt < 0.0f) {
        dt = 0.0f;
    }

    if (zooming_) {
        ++steps_done_;
        double t = static_cast<double>(steps_done_) / static_cast<double>(std::max(1, steps_total_));
        t = std::clamp(t, 0.0, 1.0);
        double s = start_scale_ + (target_scale_ - start_scale_) * t;
        scale_ = static_cast<float>(std::max(0.0001, s));

        if (pan_override_) {
            const double cx = static_cast<double>(start_center_.x) + (static_cast<double>(target_center_.x) - static_cast<double>(start_center_.x)) * t;
            const double cy = static_cast<double>(start_center_.y) + (static_cast<double>(target_center_.y) - static_cast<double>(start_center_.y)) * t;
            SDL_Point new_center{
                static_cast<int>(std::lround(cx)), static_cast<int>(std::lround(cy)) };
            set_screen_center(new_center);
        }

        if (steps_done_ >= steps_total_) {
            scale_ = static_cast<float>(target_scale_);
            if (pan_override_) {
                set_screen_center(target_center_);
            }
            zooming_      = false;
            pan_override_ = false;
            steps_total_  = 0;
            steps_done_   = 0;
            start_scale_  = target_scale_;
        }
    }

    const float safe_sx = static_cast<float>(screen_center_.x);
    const float safe_sy = static_cast<float>(screen_center_.y);
    const float safe_ss = std::max(0.0001f, scale_);

    smoothed_center_.x = std::clamp(safe_sx, -1e8f, 1e8f);
    smoothed_center_.y = std::clamp(safe_sy, -1e8f, 1e8f);
    smoothed_scale_ = static_cast<float>(std::clamp(static_cast<double>(safe_ss), 0.0001, static_cast<double>(WarpedScreenGrid::kMaxZoomAnchors)));

    recompute_current_view();
}

double WarpedScreenGrid::compute_room_scale_from_area(const Room* room) const {
    if (!room || !room->room_area || starting_area_ <= 0.0) {
        return BASE_RATIO;
    }

    Area adjusted = convert_area_to_aspect(*room->room_area);
    double a = adjusted.get_size();
    if (a <= 0.0 || room->type == "trail") {
        return BASE_RATIO * 0.8;
    }

    double s = (a / starting_area_) * BASE_RATIO;
    s = std::clamp(s, BASE_RATIO * 0.9, BASE_RATIO * 1.05);
    return s;
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

void WarpedScreenGrid::update_zoom(Room* cur,
                         CurrentRoomFinder* finder,
                         Asset* player,
                         bool refresh_requested,
                         float dt,
                         bool dev_mode)
{
    pan_offset_x_ = 0.0;
    pan_offset_y_ = 0.0;

    if (!pan_override_) {

        if (player && !dev_mode) {
            set_screen_center(SDL_Point{ player->pos.x, player->pos.y }, false);
        } else if (focus_override_) {
            set_screen_center(focus_point_);
        } else if (cur && cur->room_area) {
            set_screen_center(cur->room_area->get_center());
        }
    }

    if (!refresh_requested && !zooming_) {
        update(dt);
        return;
    }

    if (!starting_room_ && cur && cur->room_area) {
        starting_room_ = cur;
        Area adjusted = convert_area_to_aspect(*cur->room_area);
        starting_area_ = adjusted.get_size();
        if (starting_area_ <= 0.0) starting_area_ = 1.0;
    }

    update(dt);

    if (!cur) return;
    if (manual_zoom_override_) {
        return;
    }

    Room* neigh = nullptr;
    if (finder) {
        neigh = finder->getNeighboringRoom(cur);
    }
    if (!neigh) neigh = cur;

    const double sa = compute_room_scale_from_area(cur);
    const double sb = compute_room_scale_from_area(neigh);
    double target_zoom = sa;

    if (player && cur && cur->room_area && neigh && neigh->room_area) {
        auto [ax, ay] = cur->room_area->get_center();
        auto [bx, by] = neigh->room_area->get_center();
        const double pax = double(player->pos.x);
        const double pay = double(player->pos.y);

        const double vx = double(bx - ax);
        const double vy = double(by - ay);
        const double wx = double(pax - ax);
        const double wy = double(pay - ay);
        const double vlen2 = vx * vx + vy * vy;

        double t = (vlen2 > 0.0) ? ((wx * vx + wy * vy) / vlen2) : 0.0;
        t = std::clamp(t, 0.0, 1.0);

        target_zoom = (sa * (1.0 - t)) + (sb * t);
    }

    target_zoom = std::clamp( target_zoom, static_cast<double>(settings_.zoom_low), static_cast<double>(settings_.zoom_high) );

    const bool idle = !zooming_;
    if (idle || std::fabs(target_zoom - target_scale_) > SCALE_EPS) {
        zoom_to_scale(target_zoom, 35);
    }
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
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, smoothed_scale_);

    std::vector<SDL_FPoint> ground_points;
    ground_points.reserve(4);
    const std::array<std::pair<double, double>, 4> ndc_corners{
        std::pair<double, double>{-1.0, -1.0},
        std::pair<double, double>{ 1.0, -1.0},
        std::pair<double, double>{ 1.0,  1.0},
        std::pair<double, double>{-1.0,  1.0}
    };
    for (const auto& [nx, ny] : ndc_corners) {
        auto gp = project_ndc_to_ground(cam, nx, ny);
        if (gp.has_value()) {
            ground_points.push_back(*gp);
        }
    }

    if (ground_points.empty()) {
        SDL_Point center{
            static_cast<int>(std::lround(smoothed_center_.x)), static_cast<int>(std::lround(smoothed_center_.y)) };
        current_view_ = make_rect_area("current_view", center, screen_width_, screen_height_, 0);
        update_geometry_cache(compute_geometry());
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

    const float margin = std::max(0.0f, settings_.extra_cull_margin);
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
    update_geometry_cache(compute_geometry());
}

void WarpedScreenGrid::pan_and_zoom_to_point(SDL_Point world_pos, double zoom_scale_factor, int duration_steps) {
    focus_override_ = true;
    focus_point_    = world_pos;

    const double factor    = (zoom_scale_factor > 0.0) ? zoom_scale_factor : 1.0;
    const double new_scale = clamp_zoom_scale(static_cast<double>(scale_) * factor);

    if (duration_steps <= 0) {
        manual_zoom_override_ = true;
        pan_override_         = false;
        zooming_              = false;
        steps_total_          = 0;
        steps_done_           = 0;
        start_scale_          = new_scale;
        target_scale_         = new_scale;
        set_screen_center(world_pos);
        set_scale(static_cast<float>(new_scale));
        recompute_current_view();
        return;
    }

    start_center_  = screen_center_;
    target_center_ = world_pos;
    start_scale_   = scale_;
    target_scale_  = new_scale;
    steps_total_   = std::max(1, duration_steps);
    steps_done_    = 0;
    zooming_       = true;
    pan_override_  = true;
    manual_zoom_override_ = true;
}

void WarpedScreenGrid::pan_and_zoom_to_asset(const Asset* a, double zoom_scale_factor, int duration_steps) {
    if (!a) return;
    SDL_Point target{ a->pos.x, a->pos.y };
    pan_and_zoom_to_point(target, zoom_scale_factor, duration_steps);
}

void WarpedScreenGrid::animate_zoom_multiply(double factor, int duration_steps) {
    if (factor <= 0.0) factor = 1.0;
    const double new_scale = clamp_zoom_scale(static_cast<double>(scale_) * factor);

    if (duration_steps <= 0) {
        manual_zoom_override_ = true;
        pan_override_         = false;
        zooming_              = false;
        steps_total_          = 0;
        steps_done_           = 0;
        start_scale_          = new_scale;
        target_scale_         = new_scale;
        start_center_         = screen_center_;
        target_center_        = screen_center_;
        set_scale(static_cast<float>(new_scale));
        recompute_current_view();
        return;
    }

    start_center_  = screen_center_;
    target_center_ = screen_center_;
    start_scale_   = scale_;
    target_scale_  = new_scale;
    steps_total_   = std::max(1, duration_steps);
    steps_done_    = 0;
    zooming_       = true;
    pan_override_  = false;
    manual_zoom_override_ = true;
}

void WarpedScreenGrid::animate_zoom_towards_point(double factor, SDL_Point screen_point, int duration_steps) {
    if (factor <= 0.0) {
        factor = 1.0;
    }

    const double current_scale = clamp_zoom_scale(static_cast<double>(scale_));
    const double new_scale     = clamp_zoom_scale(current_scale * factor);

    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    std::tie(minx, miny, maxx, maxy) = current_view_.get_bounds();

    const double world_x = static_cast<double>(minx) + static_cast<double>(screen_point.x) * current_scale;
    const double world_y = static_cast<double>(maxy) - static_cast<double>(screen_point.y) * current_scale;

    const int base_w = std::max(1, width_from_area(base_zoom_));
    const int base_h = std::max(1, height_from_area(base_zoom_));

    const double anchored_center_x =
        world_x - static_cast<double>(screen_point.x) * new_scale + (static_cast<double>(base_w) * new_scale) * 0.5;
    const double anchored_center_y =
        world_y + static_cast<double>(screen_point.y) * new_scale - (static_cast<double>(base_h) * new_scale) * 0.5;

    constexpr double PAN_GAIN = 2.0;
    const double dx = anchored_center_x - static_cast<double>(screen_center_.x);
    const double dy = anchored_center_y - static_cast<double>(screen_center_.y);
    const double target_center_x = static_cast<double>(screen_center_.x) + dx * PAN_GAIN;
    const double target_center_y = static_cast<double>(screen_center_.y) + dy * PAN_GAIN;

    SDL_Point target_center{
        static_cast<int>(std::lround(target_center_x)), static_cast<int>(std::lround(target_center_y)) };

    if (duration_steps <= 0) {
        manual_zoom_override_ = true;
        pan_override_         = false;
        zooming_              = false;
        steps_total_          = 0;
        steps_done_           = 0;
        start_scale_          = new_scale;
        target_scale_         = new_scale;
        start_center_         = screen_center_;
        target_center_        = target_center;
        set_screen_center(target_center);
        set_scale(static_cast<float>(new_scale));
        recompute_current_view();
        return;
    }

    start_center_  = screen_center_;
    target_center_ = target_center;
    start_scale_   = scale_;
    target_scale_  = new_scale;
    steps_total_   = std::max(1, duration_steps);
    steps_done_    = 0;
    zooming_       = true;
    pan_override_  = true;
    manual_zoom_override_ = true;
}

SDL_FPoint WarpedScreenGrid::map_to_screen(SDL_Point world) const {
    SDL_FPoint world_f{ static_cast<float>(world.x), static_cast<float>(world.y) };
    return map_to_screen_f(world_f);
}

SDL_FPoint WarpedScreenGrid::map_to_screen_f(SDL_FPoint world) const {
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, smoothed_scale_);
    ProjectionResult proj = project_world_point(cam,
                                                static_cast<double>(world.x),
                                                static_cast<double>(world.y),
                                                0.0,
                                                screen_width_,
                                                screen_height_,
                                                settings_.horizon_fade_band_px);
    if (!proj.valid) {
        return SDL_FPoint{0.0f, 0.0f};
    }
    return proj.screen;
}

SDL_FPoint WarpedScreenGrid::screen_to_map(SDL_Point screen) const {
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, smoothed_scale_);
    if (!cam.valid) {
        return SDL_FPoint{0.0f, 0.0f};
    }

    const double ndc_x = ((static_cast<double>(screen.x) / std::max(1, screen_width_)) * 2.0) - 1.0;
    const double ndc_y = 1.0 - ((static_cast<double>(screen.y) / std::max(1, screen_height_)) * 2.0);

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

    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, smoothed_scale_);
    ProjectionResult proj = project_world_point(cam,
                                                static_cast<double>(world.x),
                                                static_cast<double>(world.y),
                                                static_cast<double>(world_z),
                                                screen_width_,
                                                screen_height_,
                                                settings_.horizon_fade_band_px);
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

void WarpedScreenGrid::apply_camera_settings(const nlohmann::json& data) {
    if (!data.is_object()) {
        return;
    }

    const auto try_read_number = [&](const char* key, auto& target) -> bool {
        auto it = data.find(key);
        if (it == data.end() || !it->is_number()) {
            return false;
        }
        if constexpr (std::is_integral_v<std::decay_t<decltype(target)>>) {
            target = static_cast<std::decay_t<decltype(target)>>(std::lround(it->get<double>()));
        } else {
            target = static_cast<std::decay_t<decltype(target)>>(it->get<double>());
        }
        return true;
};

    const auto try_read_bool = [&](const char* key, bool& target) -> bool {
        auto it = data.find(key);
        if (it == data.end()) {
            return false;
        }
        if (it->is_boolean()) {
            target = it->get<bool>();
            return true;
        }
        if (it->is_number_integer()) {
            target = it->get<int>() != 0;
            return true;
        }
        return false;
};

    const auto try_read_enum = [&](const char* key, auto& target, int min_value, int max_value) -> bool {
        auto it = data.find(key);
        if (it == data.end() || !it->is_number_integer()) {
            return false;
        }
        const int raw = it->get<int>();
        if (raw < min_value || raw > max_value) {
            return false;
        }
        target = static_cast<std::decay_t<decltype(target)>>(raw);
        return true;
};

    try_read_bool("realism_enabled", realism_enabled_);

    const std::array<std::pair<const char*, float*>, 17> float_fields{ {
        { "extra_cull_margin", &settings_.extra_cull_margin },
        { "zoom_low", &settings_.zoom_low },
        { "zoom_high", &settings_.zoom_high },
        { "base_height_px", &settings_.base_height_px },
        { "min_visible_screen_ratio", &settings_.min_visible_screen_ratio },
        { "parallax_smoothing_lerp_rate", &settings_.parallax_smoothing.lerp_rate },
        { "parallax_smoothing_spring_frequency", &settings_.parallax_smoothing.spring_frequency },
        { "parallax_smoothing_max_step", &settings_.parallax_smoothing.max_step },
        { "parallax_smoothing_snap_threshold", &settings_.parallax_smoothing.snap_threshold },
        { "scale_hysteresis_margin", &settings_.scale_variant_hysteresis_margin },
        { "foreground_plane_screen_y", &settings_.foreground_plane_screen_y },
        { "background_plane_screen_y", &settings_.background_plane_screen_y },
        { "perspective_distance_at_scale_zero", &settings_.perspective_distance_at_scale_zero },
        { "perspective_distance_at_scale_hundred", &settings_.perspective_distance_at_scale_hundred },
        { "horizon_fade_band_px", &settings_.horizon_fade_band_px },
        { "depth_near_world", &settings_.depth_near_world },
        { "depth_far_world", &settings_.depth_far_world },
    } };
    for (const auto& [key, field] : float_fields) {
        try_read_number(key, *field);
    }

    const std::array<std::pair<const char*, int*>, 3> int_fields{ {
        { "render_quality_percent", &settings_.render_quality_percent },
        { "foreground_texture_max_opacity", &settings_.foreground_texture_max_opacity },
        { "background_texture_max_opacity", &settings_.background_texture_max_opacity }
    } };
    for (const auto& [key, field] : int_fields) {
        try_read_number(key, *field);
    }

    try_read_enum("parallax_smoothing_method", settings_.parallax_smoothing.method, 0, 2);
    if (!try_read_enum("texture_opacity_falloff_method", settings_.texture_opacity_falloff_method, 0, 4)) {
        settings_.texture_opacity_falloff_method = BlurFalloffMethod::Linear;
    }

    settings_.foreground_texture_max_opacity =
        std::clamp(settings_.foreground_texture_max_opacity, 0, 255);
    settings_.background_texture_max_opacity =
        std::clamp(settings_.background_texture_max_opacity, 0, 255);

    if (!std::isfinite(settings_.foreground_plane_screen_y)) {
        settings_.foreground_plane_screen_y = 1080.0f;
    } else {
        settings_.foreground_plane_screen_y =
            std::clamp(settings_.foreground_plane_screen_y, 0.0f, 4000.0f);
    }

    if (!std::isfinite(settings_.background_plane_screen_y)) {
        settings_.background_plane_screen_y = 0.0f;
    } else {
        settings_.background_plane_screen_y =
            std::clamp(settings_.background_plane_screen_y, 0.0f, 4000.0f);
    }

    if (!std::isfinite(settings_.zoom_low)) {
        settings_.zoom_low = 0.75f;
    }

    if (!std::isfinite(settings_.zoom_high)) {
        settings_.zoom_high = std::max(settings_.zoom_low + 0.25f, 1.0f);
    }

    if (!std::isfinite(settings_.base_height_px) || settings_.base_height_px <= 0.0f) {
        settings_.base_height_px = 720.0f;
    }

    if (!std::isfinite(settings_.min_visible_screen_ratio) ||
        settings_.min_visible_screen_ratio < 0.0f) {
        settings_.min_visible_screen_ratio = 0.015f;
    } else {
        settings_.min_visible_screen_ratio =
            std::clamp(settings_.min_visible_screen_ratio, 0.0f, 0.5f);
    }

    settings_.zoom_low = std::clamp(settings_.zoom_low, WarpedScreenGrid::kMinZoomAnchors, WarpedScreenGrid::kMaxZoomAnchors);
    const float min_high = std::min(WarpedScreenGrid::kMaxZoomAnchors, settings_.zoom_low + 0.0001f);
    settings_.zoom_high = std::clamp(settings_.zoom_high, min_high, WarpedScreenGrid::kMaxZoomAnchors);

    auto align_quality = [](int percent) {
        constexpr int kOptions[] = {100, 75, 50, 25, 10};
        int best = kOptions[0];
        int best_diff = std::abs(percent - best);
        for (int option : kOptions) {
            const int diff = std::abs(percent - option);
            if (diff < best_diff) {
                best_diff = diff;
                best = option;
            }
        }
        return best;
};

    settings_.render_quality_percent = align_quality(settings_.render_quality_percent);

    settings_.parallax_smoothing = sanitize_params(settings_.parallax_smoothing);
    if (!std::isfinite(settings_.scale_variant_hysteresis_margin) ||
        settings_.scale_variant_hysteresis_margin < 0.0f) {
    settings_.scale_variant_hysteresis_margin = 0.05f;
    }

    try_read_bool("depth_enabled", depth_enabled_);
    try_read_bool("depth_debug_logging", depth_debug_logging_);

    recompute_current_view();
}

nlohmann::json WarpedScreenGrid::camera_settings_to_json() const {
    nlohmann::json j = nlohmann::json::object();
    j["realism_enabled"] = realism_enabled_;

    const std::pair<const char*, float> float_fields[] = {
        { "extra_cull_margin", settings_.extra_cull_margin },
        { "zoom_low", settings_.zoom_low },
        { "zoom_high", settings_.zoom_high },
        { "perspective_distance_at_scale_zero", settings_.perspective_distance_at_scale_zero },
        { "perspective_distance_at_scale_hundred", settings_.perspective_distance_at_scale_hundred },
        { "base_height_px", settings_.base_height_px },
        { "min_visible_screen_ratio", settings_.min_visible_screen_ratio },
        { "scale_hysteresis_margin", settings_.scale_variant_hysteresis_margin },
        { "parallax_smoothing_lerp_rate", settings_.parallax_smoothing.lerp_rate },
        { "parallax_smoothing_spring_frequency", settings_.parallax_smoothing.spring_frequency },
        { "parallax_smoothing_max_step", settings_.parallax_smoothing.max_step },
        { "parallax_smoothing_snap_threshold", settings_.parallax_smoothing.snap_threshold },
        { "foreground_plane_screen_y", settings_.foreground_plane_screen_y },
        { "background_plane_screen_y", settings_.background_plane_screen_y },
        { "horizon_fade_band_px", settings_.horizon_fade_band_px },
        { "perspective_scale_gamma", settings_.perspective_scale_gamma },
        { "depth_near_world", settings_.depth_near_world },
        { "depth_far_world", settings_.depth_far_world }
};
    for (const auto& [key, value] : float_fields) {
        j[key] = value;
    }

    const std::pair<const char*, int> int_fields[] = {
        { "render_quality_percent", settings_.render_quality_percent },
        { "parallax_smoothing_method", static_cast<int>(settings_.parallax_smoothing.method) },
        { "foreground_texture_max_opacity", settings_.foreground_texture_max_opacity },
        { "background_texture_max_opacity", settings_.background_texture_max_opacity },
        { "texture_opacity_falloff_method", static_cast<int>(settings_.texture_opacity_falloff_method) }
};
    for (const auto& [key, value] : int_fields) {
        j[key] = value;
    }

    j["depth_enabled"] = depth_enabled_;
    j["depth_debug_logging"] = depth_debug_logging_;

    return j;
}
SDL_FPoint WarpedScreenGrid::get_view_center_f() const {
    if (std::isfinite(smoothed_center_.x) && std::isfinite(smoothed_center_.y)) {
        return smoothed_center_;
    }
    int left, top, right, bottom;
    std::tie(left, top, right, bottom) = current_view_.get_bounds();
    const float cx = (static_cast<float>(left) + static_cast<float>(right)) * 0.5f;
    const float cy = (static_cast<float>(top)  + static_cast<float>(bottom)) * 0.5f;
    return SDL_FPoint{ cx, cy };
}

WarpedScreenGrid::FloorDepthParams WarpedScreenGrid::compute_floor_depth_params_for_geometry(const CameraGeometry& geom, double scale_value) const {
    return build_floor_params(settings_, screen_height_, geom, scale_value, realism_enabled_);
}

WarpedScreenGrid::FloorDepthParams WarpedScreenGrid::compute_floor_depth_params_for_scale(double scale_value) const {
    const CameraGeometry geom = compute_geometry_for_scale(scale_value);
    return compute_floor_depth_params_for_geometry(geom, scale_value);
}

WarpedScreenGrid::FloorDepthParams WarpedScreenGrid::compute_floor_depth_params() const {
    const CameraGeometry geom = compute_geometry();
    return compute_floor_depth_params_for_geometry(geom, static_cast<double>(smoothed_scale_));
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

double WarpedScreenGrid::view_height_for_scale(double scale_value) const {
    const int base_h = std::max(1, height_from_area(base_zoom_));
    const double clamped_scale = std::max(0.0001, scale_value);
    return static_cast<double>(base_h) * clamped_scale;
}

double WarpedScreenGrid::anchor_world_y() const {

    return static_cast<double>(smoothed_center_.y);
}

double WarpedScreenGrid::zoom_lerp_t_for_scale(double scale_value) const {
    return ZoomInterpolator(settings_, scale_value).t;
}

float WarpedScreenGrid::depth_offset_for_scale(double scale_value) const {
    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, scale_value);
    if (!cam.valid) {
        return 0.0f;
    }
    return static_cast<float>(std::clamp(cam.reference_depth, 0.0, 1e6));
}

double WarpedScreenGrid::horizon_screen_y_for_scale_value(double scale_value) const {
    if (!realism_enabled_) {
        return 0.0;
    }

    const double extent    = static_cast<double>(screen_height_);
    const double min_bound = -4.0 * extent;
    const double max_bound = extent * 0.45;

    const CameraState cam = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, scale_value);
    if (!cam.valid) {
        return extent > 0.0 ? extent * 0.5 : 0.0;
    }
    return std::clamp(cam.horizon_screen_y, min_bound, max_bound);
}

double WarpedScreenGrid::horizon_screen_y_for_scale() const {
    return horizon_screen_y_for_scale_value(static_cast<double>(smoothed_scale_));
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

    const CameraState cam_state = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, smoothed_scale_);
    if (!cam_state.valid) {
        last_min_world_z_ = 0;
        last_max_world_z_ = 0;
        last_depth_culled_ = 0;
        rebuild_grid_bounds();
        return;
    }
    const float depth_near = static_cast<float>(std::max(0.0, cam_state.near_plane));
    const float depth_far  = static_cast<float>(std::max(cam_state.near_plane + 1.0, cam_state.far_plane));

    const float margin_px    = std::max(0.0f, settings_.extra_cull_margin);
    const float cull_top = std::clamp(static_cast<float>(cam_state.horizon_screen_y) - margin_px, 0.0f, screen_h);
    const SDL_FRect cull_rect{
        -margin_px,
        cull_top,
        screen_w + margin_px * 2.0f,
        screen_h - cull_top + margin_px
};
    const float min_visible_px =
        screen_h * std::clamp(settings_.min_visible_screen_ratio, 0.0f, 0.5f);
    last_min_world_z_ = std::numeric_limits<int>::max();
    last_max_world_z_ = std::numeric_limits<int>::min();
    last_depth_culled_ = 0;

    auto rects_intersect = [](const SDL_FRect& a, const SDL_FRect& b) -> bool {
        const float ax1 = a.x + a.w;
        const float ay1 = a.y + a.h;
        const float bx1 = b.x + b.w;
        const float by1 = b.y + b.h;
        return !(ax1 < b.x || bx1 < a.x || ay1 < b.y || by1 < a.y);
};

    for (world::GridPoint* gp : grid_points) {
        if (!gp) continue;
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
        const ProjectionResult proj = project_world_point(
            cam_state,
            static_cast<double>(world_pos.x),
            static_cast<double>(world_pos.y),
            static_cast<double>(gp->world_z()),
            screen_width_,
            screen_height_,
            settings_.horizon_fade_band_px);
        if (!proj.valid) {
            continue;
        }

        SDL_FPoint screen_pos = proj.screen;
        if (!std::isfinite(screen_pos.x) || !std::isfinite(screen_pos.y)) {
            continue;
        }

        float base_scale = primary_asset->smoothed_scale();
        if (!std::isfinite(base_scale) || base_scale <= 0.0f) {
            base_scale = 1.0f;
        }

        const int fw = (primary_asset && primary_asset->info) ? std::max(1, primary_asset->info->original_canvas_width) : 1;
        const int fh = (primary_asset && primary_asset->info) ? std::max(1, primary_asset->info->original_canvas_height) : 1;

        float approx_w = static_cast<float>(fw) * base_scale * proj.perspective_scale;
        float approx_h = static_cast<float>(fh) * base_scale * proj.perspective_scale * proj.vertical_scale;
        const float min_size = std::max(1.0f, min_visible_px);
        approx_w = std::isfinite(approx_w) && approx_w > 0.0f ? std::max(approx_w, min_size) : min_size;
        approx_h = std::isfinite(approx_h) && approx_h > 0.0f ? std::max(approx_h, min_size) : min_size;

        SDL_FRect bounds{
            screen_pos.x - approx_w * 0.5f,
            screen_pos.y - approx_h,
            approx_w,
            approx_h
};

        const float distance_to_cam = proj.distance;
        const bool depth_ok = distance_to_cam >= depth_near && distance_to_cam <= depth_far;
        const bool intersects = rects_intersect(bounds, cull_rect);
        const bool has_alpha  = proj.horizon_fade > 0.001f;
        const bool on_screen  = intersects && has_alpha && depth_ok;

        last_min_world_z_ = std::min(last_min_world_z_, gp->world_z());
        last_max_world_z_ = std::max(last_max_world_z_, gp->world_z());
        if (!depth_ok) {
            ++last_depth_culled_;
        }

        for (const auto& owned : gp->occupants) {
            if (owned) {
                asset_to_point_[owned.get()] = gp;
            }
        }

        gp->screen             = screen_pos;
        gp->parallax_dx        = 0.0f;
        gp->vertical_scale     = proj.vertical_scale;
        gp->horizon_fade_alpha = proj.horizon_fade;

        gp->perspective_scale  = proj.perspective_scale;
        gp->distance_to_camera = distance_to_cam;
        gp->tilt_radians       = static_cast<float>(runtime_pitch_rad_);
        gp->on_screen          = on_screen;
        gp->mark_screen_data_updated(frame_stamp);

        warped_points_.push_back(gp);
        if (on_screen) {
            visible_points_.push_back(gp);
            for (const auto& owned : gp->occupants) {
                if (owned) {
                    visible_assets_.push_back(owned.get());
                }
            }
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
    focus_override_ = true;
    focus_point_ = focus;
}

void WarpedScreenGrid::set_manual_zoom_override(bool enabled) {
    manual_zoom_override_ = enabled;
}

void WarpedScreenGrid::clear_focus_override() {
    focus_override_ = false;
}

void WarpedScreenGrid::clear_manual_zoom_override() {
    manual_zoom_override_ = false;
}

double WarpedScreenGrid::default_zoom_for_room(const Room* room) const {
    return compute_room_scale_from_area(room);
}

void WarpedScreenGrid::project_to_screen(world::GridPoint& point) const {

    const CameraState cam_state = build_camera_state(
        settings_, aspect_, screen_width_, screen_height_, smoothed_center_, smoothed_scale_);
    const ProjectionResult proj = project_world_point(
        cam_state,
        static_cast<double>(point.world_x()),
        static_cast<double>(point.world_y()),
        static_cast<double>(point.world_z()),
        screen_width_,
        screen_height_,
        settings_.horizon_fade_band_px);

    if (!proj.valid) {
        point.screen = SDL_FPoint{0.0f, 0.0f};
        point.parallax_dx = 0.0f;
        point.on_screen = false;
        return;
    }

    point.screen          = proj.screen;
    point.parallax_dx     = 0.0f;
    point.vertical_scale  = proj.vertical_scale;
    point.perspective_scale = proj.perspective_scale;
    point.horizon_fade_alpha = proj.horizon_fade;
    point.distance_to_camera = proj.distance;
    point.tilt_radians    = static_cast<float>(runtime_pitch_rad_);
    point.on_screen       = true;
}

