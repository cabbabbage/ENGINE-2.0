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
    const double safe_height = std::max(1.0, std::isfinite(fallback_height_px) ? fallback_height_px : 1000.0);
    if (!std::isfinite(params.height_px) || params.height_px <= 0.0) {
        params.height_px = safe_height;
    }
    params.height_px = std::max(1.0, params.height_px);
    params.tilt_deg = sanitize_pitch_degrees(static_cast<float>(params.tilt_deg));
    if (!std::isfinite(params.y_distance_px)) {
        params.y_distance_px = 0.0;
    }
    params.y_distance_px = std::clamp(params.y_distance_px, -100000.0, 100000.0);
    if (!std::isfinite(params.zoom_percent)) {
        params.zoom_percent = 0.0;
    }
    params.zoom_percent = std::clamp(params.zoom_percent, 0.0, 100.0);
    if (!std::isfinite(params.pan_y_percent)) {
        params.pan_y_percent = 0.0;
    }
    params.pan_y_percent = std::clamp(params.pan_y_percent, -100.0, 100.0);
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
    start_ = smoothed_;
    target_ = smoothed_;
    manual_scale_ = smoothed_.height_px;
    manual_height_override_ = false;
    focus_override_ = false;
    animating_ = false;
    steps_total_ = 0;
    steps_done_ = 0;
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

void CameraController::set_params(const CameraParams& params) {
    smoothed_ = camera_math::sanitize_camera_params(params, fallback_height_px_);
    start_ = smoothed_;
    target_ = smoothed_;
    animating_ = false;
    steps_total_ = 0;
    steps_done_ = 0;
    manual_scale_ = smoothed_.height_px;
    sync_state();
}

void CameraController::animate_height_to(double target_height_px, int steps) {
    CameraParams target = smoothed_;
    target.height_px = target_height_px;
    start_animation_to(target, steps);
}

void CameraController::animate_height_multiply(double factor) {
    if (!std::isfinite(factor) || factor <= 0.0) return;
    manual_height_override_ = true;
    CameraParams target = smoothed_;
    target.height_px = smoothed_.height_px * factor;
    start_animation_to(target, 10);
    sync_state();
}

void CameraController::animate_height_towards_point(double target_height_px, SDL_Point target_point) {
    manual_height_override_ = true;
    set_screen_center(target_point);
    set_focus_override(target_point);
    CameraParams target = smoothed_;
    target.height_px = target_height_px;
    start_animation_to(target, 10);
    sync_state();
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
    blended.y_distance_px = lerp(cur_params.y_distance_px, neigh_params.y_distance_px, t);
    blended.zoom_percent = lerp(cur_params.zoom_percent, neigh_params.zoom_percent, t);
    blended.pan_y_percent = lerp(cur_params.pan_y_percent, neigh_params.pan_y_percent, t);
    blended = camera_math::sanitize_camera_params(blended, fallback_height_px_);

    if (manual_height_override_ && dev_mode) {
        // Preserve manual zoom adjustments (e.g., scroll wheel) instead of snapping back to the last applied height.
        blended.height_px = animating_ ? target_.height_px : smoothed_.height_px;
    }

    const bool restart = refresh_requested || !animating_ || blended != target_;
    if (restart) {
        start_animation_to(blended, steps);
    }
    tick(0.0f);
}

void CameraController::tick(float /*dt_seconds*/) {
    if (animating_) {
        ++steps_done_;
        double anim_t = std::clamp(static_cast<double>(steps_done_) / std::max(1.0, static_cast<double>(steps_total_)), 0.0, 1.0);
        smoothed_.height_px = lerp(start_.height_px, target_.height_px, anim_t);
        smoothed_.tilt_deg = lerp(start_.tilt_deg, target_.tilt_deg, anim_t);
        smoothed_.y_distance_px = lerp(start_.y_distance_px, target_.y_distance_px, anim_t);
        smoothed_.zoom_percent = lerp(start_.zoom_percent, target_.zoom_percent, anim_t);
        smoothed_.pan_y_percent = lerp(start_.pan_y_percent, target_.pan_y_percent, anim_t);
        if (steps_done_ >= steps_total_) {
            smoothed_ = target_;
            animating_ = false;
        }
    }

    smoothed_ = camera_math::sanitize_camera_params(smoothed_, fallback_height_px_);
    manual_scale_ = smoothed_.height_px;
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
    return animating_;
}

void CameraController::start_animation_to(const CameraParams& target, int steps) {
    start_ = smoothed_;
    target_ = camera_math::sanitize_camera_params(target, fallback_height_px_);
    steps_total_ = std::max(1, steps);
    steps_done_ = 0;
    animating_ = true;
}

void CameraController::sync_state() {
    state_.params = smoothed_;
    state_.center = smoothed_center_;
    state_.focus_override = focus_point_;
    state_.has_focus_override = focus_override_;
    state_.manual_height_override = manual_height_override_;
    state_.manual_scale = manual_scale_;
    state_.pitch_deg = camera_math::sanitize_pitch_degrees(static_cast<float>(smoothed_.tilt_deg));
    state_.pitch_rad = static_cast<double>(state_.pitch_deg) * (PI_D / 180.0);
}
