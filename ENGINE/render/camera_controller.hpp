#pragma once

#include <SDL.h>
#include <cstddef>

struct CameraParams {
    double height_px = 1000.0;
    double tilt_deg = 60.0;
    double y_distance_px = 0.0;
    double zoom_percent = 0.0;
    double pan_y_percent = 0.0;

    bool operator==(const CameraParams& o) const {
        return height_px == o.height_px &&
               tilt_deg == o.tilt_deg &&
               y_distance_px == o.y_distance_px &&
               zoom_percent == o.zoom_percent &&
               pan_y_percent == o.pan_y_percent;
    }
    bool operator!=(const CameraParams& o) const { return !(*this == o); }
};

namespace camera_math {
    inline constexpr float kDefaultCameraTiltDeg = 60.0f;

    CameraParams sanitize_camera_params(const CameraParams& raw, double fallback_height_px);
    float sanitize_pitch_degrees(float raw_value, bool* clamped = nullptr);
}

class CameraController {
public:
    struct State {
        CameraParams params{};
        SDL_FPoint center{0.0f, 0.0f};
        SDL_Point focus_override{0, 0};
        bool has_focus_override = false;
        bool manual_height_override = false;
        double manual_scale = 1.0;
        float pitch_deg = 0.0f;
        double pitch_rad = 0.0;
    };

    explicit CameraController(double fallback_height_px = 1000.0);

    void reset(const CameraParams& params, SDL_Point center, double fallback_height_px);
    void set_fallback_height(double fallback_height_px);

    void set_screen_center(SDL_Point p, bool snap_immediately = true);
    SDL_Point screen_center() const;
    SDL_FPoint smoothed_center() const;

    void set_focus_override(SDL_Point focus);
    void clear_focus_override();
    bool has_focus_override() const;

    void set_manual_height_override(bool enabled);
    bool manual_height_override() const;

    void set_params(const CameraParams& params);

    void animate_height_to(double target_height_px, int steps = 10);
    void animate_height_multiply(double factor);
    void animate_height_towards_point(double target_height_px, SDL_Point target_point);

    void apply_room_targets(const CameraParams& cur,
                            const CameraParams& neigh,
                            double blend_t,
                            bool refresh_requested,
                            int steps,
                            bool dev_mode);

    void tick(float dt_seconds);

    const State& state() const;
    CameraParams current_params() const;
    double current_height() const;
    double current_pitch_rad() const;
    float current_pitch_deg() const;
    bool is_animating() const;

private:
    void sync_state();

    CameraParams smoothed_{};

    SDL_Point screen_center_{0, 0};
    SDL_FPoint smoothed_center_{0.0f, 0.0f};
    bool screen_center_initialized_ = false;
    bool focus_override_ = false;
    SDL_Point focus_point_{0, 0};
    bool manual_height_override_ = false;
    double manual_scale_ = 1.0;
    double fallback_height_px_ = 1000.0;

    State state_{};
};
