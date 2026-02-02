#include "grid_point.hpp"
#include "asset/Asset.hpp"

#include <algorithm>
#include <cmath>

namespace world {

namespace {
    // Vec3-like operations for projection math
    struct Vec3 {
        double x = 0.0, y = 0.0, z = 0.0;
    };

    inline Vec3 operator-(const Vec3& a, const Vec3& b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }

    inline double dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline double length(const Vec3& v) {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }
} // namespace

void GridPoint::project_to_screen(const CameraProjectionParams& params) {
    // Track which camera state we're using
    last_camera_state_version_ = params.state_version;

    // Convert world position to meters
    const double safe_scale = std::max(1e-6, params.meters_scale);
    const Vec3 world_meters{
        (static_cast<double>(world_x_) - params.anchor_world_x) * safe_scale,
        (static_cast<double>(world_y_) - params.anchor_world_y) * safe_scale,
        static_cast<double>(world_z_) * safe_scale
    };

    // Camera vectors
    const Vec3 cam_pos{params.position_x, params.position_y, params.position_z};
    const Vec3 cam_forward{params.forward_x, params.forward_y, params.forward_z};
    const Vec3 cam_right{params.right_x, params.right_y, params.right_z};
    const Vec3 cam_up{params.up_x, params.up_y, params.up_z};

    // Transform to camera space
    const Vec3 to_point = world_meters - cam_pos;
    const double depth_along_forward = dot(to_point, cam_forward);
    const double distance = length(to_point);

    // Early rejection for points behind or outside clipping planes
    if (depth_along_forward <= params.near_plane ||
        distance < params.near_plane ||
        distance > params.far_plane ||
        !std::isfinite(distance)) {
        screen = SDL_FPoint{0.0f, 0.0f};
        parallax_dx = 0.0f;
        on_screen = false;
        screen_data_valid = true;
        return;
    }

    // Project to camera space
    const double cam_x = dot(to_point, cam_right);
    const double cam_y = dot(to_point, cam_up);

    // Project to NDC
    const double tan_fov_x = std::max(1e-6, params.tan_half_fov_x);
    const double tan_fov_y = std::max(1e-6, params.tan_half_fov_y);
    const double ndc_x = (cam_x / depth_along_forward) / tan_fov_x;
    const double ndc_y = (cam_y / depth_along_forward) / tan_fov_y;

    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
        screen = SDL_FPoint{0.0f, 0.0f};
        parallax_dx = 0.0f;
        on_screen = false;
        screen_data_valid = true;
        return;
    }

    // NDC to screen (with zoom)
    const double scaled_x = ndc_x * params.screen_zoom;
    const double scaled_y = ndc_y * params.screen_zoom;
    const double screen_x = (scaled_x * 0.5 + 0.5) * static_cast<double>(params.screen_width);
    const double screen_y = (0.5 - scaled_y * 0.5) * static_cast<double>(params.screen_height) + params.screen_pan_y_px;

    // Calculate perspective scale
    const double safe_width = static_cast<double>(std::max(1, params.screen_width));
    const double pixels_per_meter = (safe_width * 0.5) * params.screen_zoom / tan_fov_x;
    double scale = params.meters_scale * pixels_per_meter / depth_along_forward;

    // Calculate vertical scale by projecting unit vectors
    float vert_scale = 1.0f;
    const double unit_meters = std::max(1e-6, params.meters_scale);
    const Vec3 unit_world_x{unit_meters, 0.0, 0.0};
    const Vec3 unit_world_z{0.0, 0.0, unit_meters};

    auto screen_distance_for_delta = [&](const Vec3& unit_world) -> std::optional<double> {
        const Vec3 delta_cam{
            dot(unit_world, cam_right),
            dot(unit_world, cam_up),
            dot(unit_world, cam_forward)
        };
        const double depth2 = depth_along_forward + delta_cam.z;
        if (!std::isfinite(depth2) || depth2 <= params.near_plane) {
            return std::nullopt;
        }
        const double ndc_x2 = ((cam_x + delta_cam.x) / depth2) / tan_fov_x;
        const double ndc_y2 = ((cam_y + delta_cam.y) / depth2) / tan_fov_y;
        if (!std::isfinite(ndc_x2) || !std::isfinite(ndc_y2)) {
            return std::nullopt;
        }
        const double sx2 = (ndc_x2 * params.screen_zoom * 0.5 + 0.5) * safe_width;
        const double sy2 = (0.5 - ndc_y2 * params.screen_zoom * 0.5) * params.screen_height + params.screen_pan_y_px;
        const double dx = sx2 - (scaled_x * 0.5 + 0.5) * safe_width;
        const double dy = sy2 - screen_y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(dist) || dist <= 1e-6) {
            return std::nullopt;
        }
        return dist;
    };

    if (const auto scale_x = screen_distance_for_delta(unit_world_x)) {
        scale = *scale_x;
    }
    if (const auto scale_z = screen_distance_for_delta(unit_world_z)) {
        if (std::isfinite(scale) && scale > 1e-6) {
            vert_scale = static_cast<float>(*scale_z / scale);
        }
    }

    // Clamp perspective scale
    if (params.near_camera_max_perspective_scale > 0.0f) {
        scale = std::min(scale, static_cast<double>(params.near_camera_max_perspective_scale));
    }
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    if (!std::isfinite(vert_scale) || vert_scale <= 0.0f) {
        vert_scale = 1.0f;
    }

    // Calculate horizon fade
    const float zoom_scale = std::isfinite(params.screen_zoom) && params.screen_zoom > 0.0
        ? static_cast<float>(params.screen_zoom) : 1.0f;
    const float effective_horizon_band = params.horizon_band_px * zoom_scale;

    float h_fade = 1.0f;
    if (effective_horizon_band > 0.0f) {
        const float dist_from_horizon = static_cast<float>(screen_y - params.horizon_screen_y);
        if (dist_from_horizon <= 0.0f) {
            h_fade = 0.0f;
        } else if (dist_from_horizon < effective_horizon_band) {
            const float t = dist_from_horizon / effective_horizon_band;
            h_fade = std::clamp(t * t * t, 0.0f, 1.0f);
        }
    }

    // Calculate near-camera fade
    float near_fade = 1.0f;
    if (std::isfinite(screen_y) && params.screen_height > 0) {
        const float screen_h = static_cast<float>(params.screen_height);
        const float offscreen_amount = std::max(0.0f, params.offscreen_fade_amount_px);
        if (static_cast<float>(screen_y) >= screen_h) {
            if (offscreen_amount <= 0.0f) {
                near_fade = 0.0f;
            } else {
                const float t = (static_cast<float>(screen_y) - screen_h) / offscreen_amount;
                near_fade = std::clamp(1.0f - t, 0.0f, 1.0f);
            }
        }
    }

    // Store results
    screen = SDL_FPoint{static_cast<float>(screen_x), static_cast<float>(screen_y)};
    parallax_dx = 0.0f;
    vertical_scale = vert_scale;
    perspective_scale = static_cast<float>(std::max(0.0001, scale));
    horizon_fade_alpha = h_fade;
    near_camera_fade_alpha = near_fade;
    distance_to_camera = static_cast<float>(distance);
    tilt_radians = static_cast<float>(params.pitch_radians);
    on_screen = std::isfinite(screen_x) && std::isfinite(screen_y);
    screen_data_valid = true;
}

GridPoint& GridPoint::operator=(GridPoint&& other) noexcept {
    if (this == &other) return *this;

    // Transfer screen data (mutable per-frame state)
    screen = other.screen;
    parallax_dx = other.parallax_dx;
    vertical_scale = other.vertical_scale;
    horizon_fade_alpha = other.horizon_fade_alpha;
    near_camera_fade_alpha = other.near_camera_fade_alpha;
    perspective_scale = other.perspective_scale;
    distance_to_camera = other.distance_to_camera;
    tilt_radians = other.tilt_radians;
    on_screen = other.on_screen;
    screen_data_frame_updated = other.screen_data_frame_updated;
    screen_data_valid = other.screen_data_valid;
    last_camera_state_version_ = other.last_camera_state_version_;

    // Transfer occupants (ownership)
    occupants = std::move(other.occupants);

    // Transfer hierarchy tracking
    children_with_assets = other.children_with_assets;
    active_child_mask = other.active_child_mask;

    // Transfer child pointers
    x_child_neg_ = other.x_child_neg_;
    x_child_pos_ = other.x_child_pos_;
    y_child_neg_ = other.y_child_neg_;
    y_child_pos_ = other.y_child_pos_;
    z_child_neg_ = other.z_child_neg_;
    z_child_pos_ = other.z_child_pos_;

    // Clear source child pointers
    other.x_child_neg_ = nullptr;
    other.x_child_pos_ = nullptr;
    other.y_child_neg_ = nullptr;
    other.y_child_pos_ = nullptr;
    other.z_child_neg_ = nullptr;
    other.z_child_pos_ = nullptr;
    other.children_with_assets = 0;
    other.active_child_mask = 0;

    return *this;
}

void GridPoint::update_world_position(int new_x, int new_y, int new_z) {
    world_x_ = new_x;
    world_y_ = new_y;
    world_z_ = new_z;
    // Keep legacy 'world' field in sync
    const_cast<SDL_Point&>(world) = SDL_Point{new_x, new_y};
    // Invalidate screen data since position changed
    screen_data_valid = false;
}

void swap(GridPoint& a, GridPoint& b) noexcept {
    using std::swap;

    // Swap screen data
    swap(a.screen, b.screen);
    swap(a.parallax_dx, b.parallax_dx);
    swap(a.vertical_scale, b.vertical_scale);
    swap(a.horizon_fade_alpha, b.horizon_fade_alpha);
    swap(a.near_camera_fade_alpha, b.near_camera_fade_alpha);
    swap(a.perspective_scale, b.perspective_scale);
    swap(a.distance_to_camera, b.distance_to_camera);
    swap(a.tilt_radians, b.tilt_radians);
    swap(a.on_screen, b.on_screen);
    swap(a.screen_data_frame_updated, b.screen_data_frame_updated);
    swap(a.screen_data_valid, b.screen_data_valid);
    swap(a.last_camera_state_version_, b.last_camera_state_version_);

    // Swap occupants
    swap(a.occupants, b.occupants);

    // Swap hierarchy tracking
    swap(a.children_with_assets, b.children_with_assets);
    swap(a.active_child_mask, b.active_child_mask);

    // Swap child pointers
    swap(a.x_child_neg_, b.x_child_neg_);
    swap(a.x_child_pos_, b.x_child_pos_);
    swap(a.y_child_neg_, b.y_child_neg_);
    swap(a.y_child_pos_, b.y_child_pos_);
    swap(a.z_child_neg_, b.z_child_neg_);
    swap(a.z_child_pos_, b.z_child_pos_);
}

} // namespace world
