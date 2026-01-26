#pragma once

#include <optional>

#include <SDL.h>

class WarpedScreenGrid;

namespace devmode::frame_editors {

class CameraController {
public:
    struct CameraLockState {
        bool valid = false;
        bool manual_override_before = false;
        bool focus_override_before = false;
        SDL_Point focus_point_before{0, 0};
        SDL_Point screen_center_before{0, 0};
        std::optional<float> tilt_override_before{};
        double camera_y_distance_before = 0.0;
    };

    void capture(WarpedScreenGrid&) { state_.valid = true; }
    void lock(WarpedScreenGrid&) {}
    void restore(WarpedScreenGrid&) { state_.valid = false; }
    void enforce(WarpedScreenGrid&) const {}

    void set_height_locked(bool locked, double y_distance) {
        height_locked_ = locked;
        locked_height_ = y_distance;
    }

    void set_tilt_locked(bool locked, float tilt) {
        tilt_locked_ = locked;
        locked_tilt_deg_ = tilt;
    }

private:
    CameraLockState state_{};
    bool height_locked_ = false;
    double locked_height_ = 0.0;
    bool tilt_locked_ = false;
    float locked_tilt_deg_ = 0.0f;
};

}  // namespace devmode::frame_editors
