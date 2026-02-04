#include "camera_controller.hpp"

#include <algorithm>
#include <cmath>

namespace {
    template <typename T>
    T lerp(T a, T b, double t) {
        return static_cast<T>(a + (b - a) * t);
    }

    constexpr double PI_D = 3.14159265358979323846;

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
}

float camera_math::sanitize_pitch_degrees(float raw_value, bool* clamped) {
    if (clamped) *clamped = false;
    const float wrapped = static_cast<float>(wrap_degrees_0_360(std::isfinite(raw_value) ? raw_value : camera_math::kDefaultCameraTiltDeg));
    const float clamped_value = std::clamp(wrapped, 0.0f, 150.0f);
    if (clamped && clamped_value != raw_value) {
        *clamped = true;
    }
    return clamped_value;
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
    smoothed_center_ = SDL_FPoint{0.0f, 0.0f};
    state_.center    = smoothed_center_;
    state_.pitch_deg = camera_math::kDefaultCameraTiltDeg;
    state_.pitch_rad = static_cast<double>(state_.pitch_deg) * (PI_D / 180.0);
}

void CameraController::reset(const CameraParams& params, SDL_Point center, double fallback_height_px) {
    fallback_height_px_ = (fallback_height_px > 0.0) ? fallback_height_px : fallback_height_px_;
    screen_center_ = center;
    screen_center_initialized_ = true;
    smoothed_center_.x = static_cast<float>(center.x);
    smoothed_center_.y = static_cast<float>(center.y);
    smoothed_ = camera_math::sanitize_camera_params(params, fallback_height_px_);
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
    }
}

void CameraController::set_screen_center(SDL_Point p, bool snap_immediately) {
    if (!screen_center_initialized_) {
        screen_center_             = p;
        screen_center_initialized_ = true;
        smoothed_center_.x         = static_cast<float>(p.x);
        smoothed_center_.y         = static_cast<float>(p.y);
        sync_state();
        return;
    }

    screen_center_ = p;
    smoothed_center_.x = static_cast<float>(p.x);
    smoothed_center_.y = static_cast<float>(p.y);
    (void)snap_immediately;
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

void CameraController::set_params(const CameraParams& params) {
    smoothed_ = camera_math::sanitize_camera_params(params, fallback_height_px_);
    manual_scale_ = smoothed_.height_px;
    manual_zoom_percent_ = smoothed_.zoom_percent;
    sync_state();
}

void CameraController::set_zoom_percent(double percent) {
    CameraParams params = smoothed_;
    params.zoom_percent = percent;
    manual_zoom_override_ = true;
    set_params(params);
}

double CameraController::zoom_percent() const {
    return smoothed_.zoom_percent;
}

void CameraController::animate_height_to(double target_height_px, int steps) {
    (void)steps;
    CameraParams target = smoothed_;
    target.height_px = target_height_px;
    set_params(target);
}

void CameraController::animate_height_multiply(double factor) {
    if (!std::isfinite(factor) || factor <= 0.0) return;
    manual_height_override_ = true;
    CameraParams target = smoothed_;
    target.height_px = smoothed_.height_px * factor;
    set_params(target);
}

void CameraController::animate_height_towards_point(double target_height_px, SDL_Point target_point) {
    manual_height_override_ = true;
    set_screen_center(target_point);
    set_focus_override(target_point);
    CameraParams target = smoothed_;
    target.height_px = target_height_px;
    set_params(target);
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
        // Preserve manual height adjustments instead of snapping back to the last applied height.
        blended.height_px = smoothed_.height_px;
    }
    if (manual_zoom_override_) {
        // Keep developer-driven zoom overrides until explicitly cleared.
        blended.zoom_percent = smoothed_.zoom_percent;
    }
    (void)dev_mode;
    (void)refresh_requested;
    (void)steps;
    set_params(blended);
}

void CameraController::tick(float /*dt_seconds*/) {
    smoothed_ = camera_math::sanitize_camera_params(smoothed_, fallback_height_px_);
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
    return false;
}

void CameraController::sync_state() {
    state_.params = smoothed_;
    state_.center = smoothed_center_;
    state_.focus_override = focus_point_;
    state_.has_focus_override = focus_override_;
    state_.manual_height_override = manual_height_override_;
    state_.manual_zoom_override = manual_zoom_override_;
    state_.manual_scale = manual_scale_;
    state_.manual_zoom_percent = manual_zoom_percent_;
    state_.pitch_deg = camera_math::sanitize_pitch_degrees(static_cast<float>(smoothed_.tilt_deg));
    state_.pitch_rad = static_cast<double>(state_.pitch_deg) * (PI_D / 180.0);
}
