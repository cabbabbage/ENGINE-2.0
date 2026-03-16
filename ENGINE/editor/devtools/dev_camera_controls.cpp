#include "dev_camera_controls.hpp"

#include "rendering/render/warped_screen_grid.hpp"
#include "utils/input.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {
constexpr float kTiltDegreesPerPixel = 0.2f;
constexpr double kZoomStepPercent = 5.0;
constexpr double kMinZoomPercent = 0.0;
constexpr double kMaxZoomPercent = 100.0;
}

void DevCameraControls::set_height_scale_factor(double factor) {
    height_scale_factor_ = (factor > 0.0) ? factor : 1.0;
}

void DevCameraControls::handle_input(WarpedScreenGrid& cam,
                                     const Input& input,
                                     bool pan_blocked,
                                     std::optional<int> scroll_override) {
    const SDL_Point mouse{ input.getX(), input.getY() };
    const int wheel_y = scroll_override.has_value() ? *scroll_override : input.getScrollY();
    const bool left_down = input.isDown(Input::LEFT);
    const bool ctrl_down =
        input.isScancodeDown(SDL_SCANCODE_LCTRL) || input.isScancodeDown(SDL_SCANCODE_RCTRL);

    if (wheel_y != 0) {
        if (ctrl_down && !pan_blocked) {
            const int ticks = std::abs(wheel_y);
            // SDL wheel up is positive. Keep controls consistent with editor docs:
            // up increases zoom/height (camera farther), down decreases.
            const double direction = (wheel_y > 0) ? 1.0 : -1.0;
            const double delta = direction * kZoomStepPercent * static_cast<double>(ticks);
            const double base_zoom = std::clamp(cam.get_zoom_percent(), kMinZoomPercent, kMaxZoomPercent);
            const double target_zoom = std::clamp(base_zoom + delta, kMinZoomPercent, kMaxZoomPercent);
            if (std::abs(target_zoom - base_zoom) > 1e-6) {
                cam.set_zoom_percent(target_zoom);
            }
        } else {
            const double step = (height_scale_factor_ > 0.0) ? height_scale_factor_ : 1.0;
            const int ticks = std::abs(wheel_y);
            const bool height_increase = (wheel_y > 0);
            const double mag = std::pow(step, ticks);
            const double eff = height_increase ? mag : (1.0 / mag);

            const double base_scale = std::max(1.0, static_cast<double>(cam.get_scale()));
            const double unclamped_target = base_scale * eff;
            const double target_scale = std::clamp(unclamped_target, 1.0, 50000.0);
            const double adjusted_eff = target_scale / base_scale;

            if (std::abs(adjusted_eff - 1.0) > 1e-6) {
                cam.set_manual_height_override(true);
                const SDL_Point focus = cam.get_screen_center();
                cam.set_focus_override(focus);
                cam.set_screen_center(focus);
                cam.animate_height_multiply(adjusted_eff);
            }
        }
    }

    if (input.wasReleased(Input::LEFT)) {
        panning_ = false;
        pan_drag_pending_ = false;
        has_last_pan_center_ = false;
        tilting_ = false;
        pan_start_world_point_ = std::nullopt;
    }

    if (input.wasPressed(Input::LEFT)) {
        if (ctrl_down) {
            if (!pan_blocked) {
                tilting_ = true;
                tilt_start_mouse_screen_ = mouse;
                const auto tilt_override = cam.tilt_override();
                const float start_tilt = tilt_override.has_value()
                    ? *tilt_override
                    : cam.current_pitch_degrees();
                tilt_start_degrees_ = std::clamp(
                    start_tilt,
                    WarpedScreenGrid::kMinPitchDegrees,
                    WarpedScreenGrid::kMaxPitchDegrees);
            } else {
                tilting_ = false;
            }
            pan_drag_pending_ = false;
            panning_ = false;
            has_last_pan_center_ = false;
        } else if (!pan_blocked) {
            pan_drag_pending_ = true;
            pan_start_mouse_screen_ = mouse;
            pan_start_center_ = cam.get_screen_center();
            pan_start_world_point_ = cam.screen_to_map(mouse);
        } else {
            panning_ = false;
            pan_drag_pending_ = false;
        }
    }

    if (!left_down) {
        pan_drag_pending_ = false;
        tilting_ = false;
        pan_start_world_point_ = std::nullopt;
    }

    if (ctrl_down && !panning_) {
        pan_drag_pending_ = false;
    }

    if (pan_blocked && !panning_) {
        pan_drag_pending_ = false;
    }

    if (tilting_) {
        if (pan_blocked || !ctrl_down || !left_down) {
            tilting_ = false;
        } else {
            const int dy = mouse.y - tilt_start_mouse_screen_.y;
            const float tilt_delta = -static_cast<float>(dy) * kTiltDegreesPerPixel;
            const float base_tilt = tilt_start_degrees_;
            const float target_tilt = std::clamp(
                base_tilt + tilt_delta,
                WarpedScreenGrid::kMinPitchDegrees,
                WarpedScreenGrid::kMaxPitchDegrees);
            if (std::abs(target_tilt - base_tilt) > 1e-4f) {
                cam.set_tilt_override(target_tilt);
            }
        }
        if (tilting_) {
            return;
        }
    }

    if (!panning_ && pan_drag_pending_ && left_down && !ctrl_down) {
        const int dx = mouse.x - pan_start_mouse_screen_.x;
        const int dy = mouse.y - pan_start_mouse_screen_.y;
        if (dx != 0 || dy != 0) {
            panning_ = true;
            pan_drag_pending_ = false;
            cam.set_focus_override(pan_start_center_);
            cam.set_screen_center(pan_start_center_);
            last_pan_center_ = pan_start_center_;
            has_last_pan_center_ = true;
        }
    }

    if (!panning_ || !left_down) {
        return;
    }

    const int dx = mouse.x - pan_start_mouse_screen_.x;
    const int dy = mouse.y - pan_start_mouse_screen_.y;
    if (dx == 0 && dy == 0) {
        return;
    }
    SDL_Point target_center = cam.get_screen_center();
    bool applied_world_lock = false;
    if (pan_start_world_point_.has_value()) {
        const SDL_FPoint current_world = cam.screen_to_map(mouse);
        if (std::isfinite(current_world.x) && std::isfinite(current_world.y)) {
            const double world_dx = pan_start_world_point_->x - current_world.x;
            const double world_dy = pan_start_world_point_->y - current_world.y;
            if (std::abs(world_dx) > 0.0 || std::abs(world_dy) > 0.0) {
                target_center.x = static_cast<int>(std::lround(target_center.x + world_dx));
                target_center.y = static_cast<int>(std::lround(target_center.y + world_dy));
                applied_world_lock = true;
            }
        }
    }
    if (!applied_world_lock) {
        target_center = SDL_Point{
            pan_start_center_.x - dx,
            pan_start_center_.y - dy
        };
    }
    if (has_last_pan_center_ &&
        target_center.x == last_pan_center_.x &&
        target_center.y == last_pan_center_.y) {
        return;
    }
    cam.set_focus_override(target_center);
    cam.set_screen_center(target_center);
    last_pan_center_ = target_center;
    has_last_pan_center_ = true;
}

void DevCameraControls::cancel(WarpedScreenGrid& cam) {
    pan_drag_pending_ = false;
    tilting_ = false;
    pan_start_world_point_ = std::nullopt;
    if (!panning_) {
        return;
    }
    panning_ = false;
    has_last_pan_center_ = false;
    cam.set_manual_height_override(false);
    cam.set_manual_zoom_override(false);
    cam.clear_focus_override();
    cam.clear_tilt_override();
}
