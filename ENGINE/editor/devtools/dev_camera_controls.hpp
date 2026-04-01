#pragma once

#include <SDL3/SDL.h>
#include <optional>

class WarpedScreenGrid;
class Input;

// Global camera height bounds for dev mode, accessed by both UI and camera controls.
struct DevCameraHeightBounds {
    static void set(double min_px, double max_px);
    static double min_px();
    static double max_px();
};

class DevCameraControls {
public:
    void set_height_scale_factor(double factor);

    void handle_input(WarpedScreenGrid& cam,
                      const Input& input,
                      bool pan_blocked,
                      std::optional<int> scroll_override = std::nullopt);

    void cancel(WarpedScreenGrid& cam);

    bool is_panning() const { return panning_; }

private:
    double height_scale_factor_ = 1.1;
    bool panning_ = false;
    bool pan_drag_pending_ = false;
    bool tilting_ = false;
    SDL_Point pan_start_mouse_screen_{0, 0};
    SDL_Point pan_start_center_{0, 0};
    SDL_Point last_pan_center_{0, 0};
    bool has_last_pan_center_ = false;
    std::optional<SDL_FPoint> pan_start_world_point_;
    SDL_Point tilt_start_mouse_screen_{0, 0};
    float tilt_start_degrees_ = 0.0f;
};

