#include "camera_controller.hpp"

#include <algorithm>
#include <cmath>

namespace {
    template <typename T>
    T lerp(T a, T b, double t) {
        return static_cast<T>(a + (b - a) * t);
    }

    constexpr double PI_D = 3.14159265358979323846;
    constexpr float kDefaultDtSeconds = 1.0f / 60.0f;
    constexpr float kCenterSnapEpsilon = 0.01f;
    constexpr double kParamSnapEpsilon = 1.0e-4;

    double wrap_degrees_0_360(double raw_value) {
        if (!std::isfinite(raw_value)) {
            return static_cast<double>(camera_math::kDefaultCameraTiltDeg);
        }
        double wrapped = std::fmod(raw_value, 360.0);
        if (wrapped < 0.0) wrapped += 360.0;
        if (wrapped >= 360.0 || !std::isfinite(wrapped)) {
            wrapped = std::fmod(wrapped, 360.0);
            if (wrapped < 0.0) wrapped += 360.0;
        }
        return std::isfinite(wrapped) ? wrapped : static_cast<double>(camera_math::kDefaultCameraTiltDeg);
    }

    float safe_dt_seconds(float dt_seconds) {
        if (std::isfinite(dt_seconds) && dt_seconds > 0.0f) {
            return std::min(dt_seconds, 0.1f);
        }
        return kDefaultDtSeconds;
    }

    double smoothing_alpha(float damping, float dt_seconds) {
        const float safe_damping = std::max(0.0f, damping);
        if (safe_damping <= 0.0f) {
            return 1.0;
        }
        return 1.0 - std::exp(-static_cast<double>(safe_damping) * static_cast<double>(dt_seconds));
    }
}

float camera_math::sanitize_pitch_degrees(float raw_value, bool* clamped) {
    if (clamped) *clamped = false;
    const float safe_input = std::isfinite(raw_value) ? raw_value : camera_math::kDefaultCameraTiltDeg;
    const float wrapped = static_cast<float>(wrap_degrees_0_360(safe_input));
    if (clamped && wrapped != raw_value) {
        *clamped = true;
    }
    return wrapped;
}

CameraParams camera_math::sanitize_camera_params(const CameraParams& raw, double fallback_height_px) {
    CameraParams params = raw;
    const double safe_height = std::isfinite(fallback_height_px) ? fallback_height_px : 1000.0;
    if (!std::isfinite(params.height_px) || params.height_px <= 0.0) {
        params.height_px = safe_height;
    }
    params.tilt_deg = sanitize_pitch_degrees(static_cast<float>(params.tilt_deg));
    if (!std::isfinite(params.zoom_percent)) {
        params.zoom_percent = 0.0;
    }
    params.zoom_percent = std::clamp(params.zoom_percent, 0.0, 100.0);

    return params;
}

CameraController::CameraController(double fallback_height_px)
    : fallback_height_px_(fallback_height_px) {
    smoothed_ = camera_math::sanitize_camera_params(smoothed_, fallback_height_px_);
    target_ = smoothed_;
    smoothed_center_ = SDL_FPoint{0.0f, 0.0f};
    target_center_ = smoothed_center_;
    center_velocity_ = SDL_FPoint{0.0f, 0.0f};
    state_.center = smoothed_center_;
    state_.pitch_deg = camera_math::kDefaultCameraTiltDeg;
    state_.pitch_rad = static_cast<double>(state_.pitch_deg) * (PI_D / 180.0);
    sync_state();
}

void CameraController::reset(const CameraParams& params, SDL_Point center, double fallback_height_px) {
    fallback_height_px_ = (fallback_height_px > 0.0) ? fallback_height_px : fallback_height_px_;
    screen_center_ = center;
    screen_center_initialized_ = true;
    smoothed_center_.x = static_cast<float>(center.x);
    smoothed_center_.y = static_cast<float>(center.y);
    target_center_ = smoothed_center_;
    center_velocity_ = SDL_FPoint{0.0f, 0.0f};
    smoothed_ = camera_math::sanitize_camera_params(params, fallback_height_px_);
    target_ = smoothed_;
    manual_scale_ = smoothed_.height_px;
    manual_height_override_ = false;
    manual_zoom_override_ = false;
    manual_zoom_percent_ = smoothed_.zoom_percent;
    focus_override_ = false;
    sync_state();
}

void CameraController::set_fallback_height(double fallback_height_px) {
    if (std::isfinite(fallback_height_px) && fallback_height_px > 0.0) {
        fallback_height_px_ = fallback_height_px;
        smoothed_ = camera_math::sanitize_camera_params(smoothed_, fallback_height_px_);
        target_ = camera_math::sanitize_camera_params(target_, fallback_height_px_);
        sync_state();
    }
}

void CameraController::set_transition_damping(float damping) {
    if (!std::isfinite(damping)) {
        return;
    }
    transition_damping_ = std::clamp(damping, 0.0f, 200.0f);
}

float CameraController::transition_damping() const {
    return transition_damping_;
}

void CameraController::set_max_camera_velocity(float px_per_second) {
    if (!std::isfinite(px_per_second)) {
        return;
    }
    max_camera_velocity_ = std::clamp(px_per_second, 1.0f, 100000.0f);
}

float CameraController::max_camera_velocity() const {
    return max_camera_velocity_;
}

void CameraController::set_transition_debug_state(int state, float blend_factor) {
    transition_state_debug_ = std::clamp(state, 0, 2);
    transition_blend_factor_debug_ = std::clamp(
        std::isfinite(blend_factor) ? blend_factor : 0.0f,
        0.0f,
        1.0f);
    sync_state();
}

void CameraController::set_screen_center(SDL_Point p, bool snap_immediately) {
    const SDL_FPoint target{
        static_cast<float>(p.x),
        static_cast<float>(p.y)
    };

    if (!screen_center_initialized_) {
        screen_center_ = p;
        screen_center_initialized_ = true;
        smoothed_center_ = target;
        target_center_ = target;
        center_velocity_ = SDL_FPoint{0.0f, 0.0f};
        sync_state();
        return;
    }

    screen_center_ = p;
    target_center_ = target;
    if (snap_immediately) {
        smoothed_center_ = target_center_;
        center_velocity_ = SDL_FPoint{0.0f, 0.0f};
    }
    sync_state();
}

SDL_Point CameraController::screen_center() const {
    return screen_center_;
}

SDL_FPoint CameraController::smoothed_center() const {
    return smoothed_center_;
}

void CameraController::set_focus_override(SDL_Point focus) {
    focus_override_ = true;
    focus_point_ = focus;
    sync_state();
}

void CameraController::clear_focus_override() {
    focus_override_ = false;
    sync_state();
}

bool CameraController::has_focus_override() const {
    return focus_override_;
}

void CameraController::set_manual_height_override(bool enabled) {
    manual_height_override_ = enabled;
    sync_state();
}

bool CameraController::manual_height_override() const {
    return manual_height_override_;
}

void CameraController::set_manual_zoom_override(bool enabled) {
    manual_zoom_override_ = enabled;
    sync_state();
}

bool CameraController::manual_zoom_override() const {
    return manual_zoom_override_;
}

void CameraController::set_target_params(const CameraParams& params) {
    target_ = camera_math::sanitize_camera_params(params, fallback_height_px_);
    sync_state();
}

void CameraController::set_params(const CameraParams& params) {
    // קריאות מפורשות מטופלות כעקיפות בסגנון דיבאג או טלפורט, ויכולות לקפוץ מיידית.
    smoothed_ = camera_math::sanitize_camera_params(params, fallback_height_px_);
    target_ = smoothed_;
    manual_scale_ = smoothed_.height_px;
    manual_zoom_percent_ = smoothed_.zoom_percent;
    sync_state();
}

void CameraController::set_zoom_percent(double percent) {
    CameraParams params = target_;
    params.zoom_percent = percent;
    manual_zoom_override_ = true;
    set_target_params(params);
}

double CameraController::zoom_percent() const {
    return smoothed_.zoom_percent;
}

void CameraController::animate_height_to(double target_height_px, int steps) {
    (void)steps;
    CameraParams target = target_;
    target.height_px = target_height_px;
    set_target_params(target);
}

void CameraController::animate_height_multiply(double factor) {
    if (!std::isfinite(factor) || factor <= 0.0) return;
    manual_height_override_ = true;
    CameraParams target = target_;
    target.height_px = target.height_px * factor;
    set_target_params(target);
}

void CameraController::animate_height_towards_point(double target_height_px, SDL_Point target_point) {
    manual_height_override_ = true;
    set_screen_center(target_point, false);
    set_focus_override(target_point);
    CameraParams target = target_;
    target.height_px = target_height_px;
    set_target_params(target);
}

void CameraController::apply_room_targets(const CameraParams& cur,
                                          const CameraParams& neigh,
                                          double blend_t,
                                          bool refresh_requested,
                                          int steps,
                                          bool dev_mode) {
    CameraParams cur_params = camera_math::sanitize_camera_params(cur, fallback_height_px_);
    CameraParams neigh_params = camera_math::sanitize_camera_params(neigh, fallback_height_px_);

    const double t = std::clamp(blend_t, 0.0, 1.0);
    CameraParams blended{};
    blended.height_px = lerp(cur_params.height_px, neigh_params.height_px, t);
    blended.tilt_deg = lerp(cur_params.tilt_deg, neigh_params.tilt_deg, t);
    blended.zoom_percent = lerp(cur_params.zoom_percent, neigh_params.zoom_percent, t);
    blended = camera_math::sanitize_camera_params(blended, fallback_height_px_);

    if (manual_height_override_) {
        blended.height_px = target_.height_px;
    }
    if (manual_zoom_override_) {
        blended.zoom_percent = target_.zoom_percent;
    }
    (void)dev_mode;
    (void)refresh_requested;
    (void)steps;
    set_target_params(blended);
}

void CameraController::tick(float dt_seconds) {
    const float safe_dt = safe_dt_seconds(dt_seconds);
    const double alpha = smoothing_alpha(transition_damping_, safe_dt);

    smoothed_ = camera_math::sanitize_camera_params(smoothed_, fallback_height_px_);
    target_ = camera_math::sanitize_camera_params(target_, fallback_height_px_);

    auto smooth_scalar = [&](double current, double target) {
        if (std::fabs(target - current) <= kParamSnapEpsilon) {
            return target;
        }
        return current + (target - current) * alpha;
    };
    smoothed_.height_px = smooth_scalar(smoothed_.height_px, target_.height_px);
    smoothed_.tilt_deg = smooth_scalar(smoothed_.tilt_deg, target_.tilt_deg);
    smoothed_.zoom_percent = smooth_scalar(smoothed_.zoom_percent, target_.zoom_percent);
    smoothed_ = camera_math::sanitize_camera_params(smoothed_, fallback_height_px_);

    const float to_target_x = target_center_.x - smoothed_center_.x;
    const float to_target_y = target_center_.y - smoothed_center_.y;
    const float distance = std::sqrt(to_target_x * to_target_x + to_target_y * to_target_y);

    if (distance <= kCenterSnapEpsilon) {
        smoothed_center_ = target_center_;
        center_velocity_ = SDL_FPoint{0.0f, 0.0f};
    } else {
        float delta_x = static_cast<float>(to_target_x * alpha);
        float delta_y = static_cast<float>(to_target_y * alpha);
        float delta_mag = std::sqrt(delta_x * delta_x + delta_y * delta_y);
        const float max_step = std::max(0.0f, max_camera_velocity_) * safe_dt;
        if (max_step > 0.0f && delta_mag > max_step && delta_mag > 1.0e-6f) {
            const float scale = max_step / delta_mag;
            delta_x *= scale;
            delta_y *= scale;
            delta_mag = max_step;
        }
        if (delta_mag > distance) {
            delta_x = to_target_x;
            delta_y = to_target_y;
        }
        smoothed_center_.x += delta_x;
        smoothed_center_.y += delta_y;
        center_velocity_.x = delta_x / safe_dt;
        center_velocity_.y = delta_y / safe_dt;
    }

    screen_center_ = SDL_Point{
        static_cast<int>(std::lround(smoothed_center_.x)),
        static_cast<int>(std::lround(smoothed_center_.y))
    };
    manual_scale_ = smoothed_.height_px;
    manual_zoom_percent_ = smoothed_.zoom_percent;
    sync_state();
}

const CameraController::State& CameraController::state() const {
    return state_;
}

CameraParams CameraController::current_params() const {
    return state_.params;
}

double CameraController::current_height() const {
    return state_.params.height_px;
}

double CameraController::current_pitch_rad() const {
    return state_.pitch_rad;
}

float CameraController::current_pitch_deg() const {
    return state_.pitch_deg;
}

bool CameraController::is_animating() const {
    const float dx = target_center_.x - smoothed_center_.x;
    const float dy = target_center_.y - smoothed_center_.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const float speed = std::sqrt(center_velocity_.x * center_velocity_.x + center_velocity_.y * center_velocity_.y);
    return dist > kCenterSnapEpsilon ||
           speed > 0.01f ||
           std::fabs(target_.height_px - smoothed_.height_px) > kParamSnapEpsilon ||
           std::fabs(target_.tilt_deg - smoothed_.tilt_deg) > kParamSnapEpsilon ||
           std::fabs(target_.zoom_percent - smoothed_.zoom_percent) > kParamSnapEpsilon;
}

void CameraController::sync_state() {
    state_.params = smoothed_;
    state_.target_params = target_;
    state_.center = smoothed_center_;
    state_.target_center = target_center_;
    state_.center_velocity = center_velocity_;
    state_.focus_override = focus_point_;
    state_.has_focus_override = focus_override_;
    state_.manual_height_override = manual_height_override_;
    state_.manual_zoom_override = manual_zoom_override_;
    state_.manual_scale = manual_scale_;
    state_.manual_zoom_percent = manual_zoom_percent_;
    state_.pitch_deg = camera_math::sanitize_pitch_degrees(static_cast<float>(smoothed_.tilt_deg));
    state_.pitch_rad = static_cast<double>(state_.pitch_deg) * (PI_D / 180.0);
    state_.transition_state = transition_state_debug_;
    state_.transition_blend_factor = transition_blend_factor_debug_;
}

