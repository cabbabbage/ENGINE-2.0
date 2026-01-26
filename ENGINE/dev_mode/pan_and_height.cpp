#include "pan_and_height.hpp"

#include "render/warped_screen_grid.hpp"
#include "utils/input.hpp"

#include <algorithm>
#include <cmath>

void PanAndHeight::set_height_scale_factor(double factor) {
    height_scale_factor_ = (factor > 0.0) ? factor : 1.0;
}

void PanAndHeight::handle_input(WarpedScreenGrid& cam,
                               const Input& input,
                               bool pan_blocked,
                               std::optional<int> scroll_override) {
    const SDL_Point mouse{ input.getX(), input.getY() };
    const int wheel_y = scroll_override.has_value() ? *scroll_override : input.getScrollY();
    const bool left_down = input.isDown(Input::LEFT);
    const bool right_down = input.isDown(Input::RIGHT);
    if (wheel_y != 0) {
        if (right_down && !pan_blocked) {
            constexpr float kTiltStepDegrees = 2.0f;
            const float tilt_delta = -static_cast<float>(wheel_y) * kTiltStepDegrees;
            const float base_tilt = cam.tilt_override_deg();
            const float target_tilt = std::clamp(
                base_tilt + tilt_delta,
                WarpedScreenGrid::kMinPitchDegrees,
                WarpedScreenGrid::kMaxPitchDegrees);
            if (std::abs(target_tilt - base_tilt) > 1e-4f) {
                cam.set_tilt_override(target_tilt);
            }
        } else {
            const double step = (height_scale_factor_ > 0.0) ? height_scale_factor_ : 1.0;
            const int ticks = std::abs(wheel_y);
            const bool height_increase = (wheel_y < 0);
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
    }

    if (input.wasPressed(Input::LEFT)) {
        if (!pan_blocked) {
            pan_drag_pending_ = true;
            pan_start_mouse_screen_ = mouse;
            pan_start_center_ = cam.get_screen_center();
        } else {
            panning_ = false;
            pan_drag_pending_ = false;
        }
    }

    if (!left_down) {
        pan_drag_pending_ = false;
    }

    if (pan_blocked && !panning_) {
        pan_drag_pending_ = false;
    }

    if (!panning_ && pan_drag_pending_ && left_down) {
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
    SDL_Point new_center{
        pan_start_center_.x - dx,
        pan_start_center_.y - dy
    };
    if (has_last_pan_center_ &&
        new_center.x == last_pan_center_.x &&
        new_center.y == last_pan_center_.y) {
        return;
    }
    cam.set_focus_override(new_center);
    cam.set_screen_center(new_center);
    last_pan_center_ = new_center;
    has_last_pan_center_ = true;
}

void PanAndHeight::cancel(WarpedScreenGrid& cam) {
    pan_drag_pending_ = false;
    if (!panning_) {
        return;
    }
    panning_ = false;
    has_last_pan_center_ = false;
    cam.set_manual_height_override(false);
    cam.clear_focus_override();
    cam.clear_tilt_override();
}
