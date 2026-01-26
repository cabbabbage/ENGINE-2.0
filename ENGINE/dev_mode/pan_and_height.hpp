#pragma once

#include <SDL.h>
#include <optional>

class WarpedScreenGrid;
class Input;

class PanAndHeight {
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
    SDL_Point pan_start_mouse_screen_{0, 0};
    SDL_Point pan_start_center_{0, 0};
    SDL_Point last_pan_center_{0, 0};
    bool has_last_pan_center_ = false;
};
