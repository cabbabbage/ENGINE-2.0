#pragma once

#include <SDL.h>

#include "SelectionState.hpp"

namespace devmode::frame_editors {

class AxisAdjuster;

class AxisPointRenderer {
public:
    // Main rendering function
    static void render_axis_point(SDL_Renderer* renderer,
                                SDL_FPoint screen_pos,
                                AdjustmentAxis axis,
                                bool is_selected,
                                float radius = 8.0f);

    // Interaction handling
    static bool handle_point_click(const SDL_Point& mouse_pos,
                                 SDL_FPoint point_screen_pos,
                                 float radius = 8.0f);

    static SDL_FPoint constrain_drag_delta(const SDL_FPoint& drag_delta,
                                         AdjustmentAxis axis);

    static void cycle_axis_on_selection(SelectionState* selection_state,
                                      AxisAdjuster* axis_adjuster);

private:
    static SDL_Color get_axis_color(AdjustmentAxis axis);
    static void render_movement_arrows(SDL_Renderer* renderer,
                                     SDL_FPoint center,
                                     AdjustmentAxis axis,
                                     float length = 16.0f);
};

}  // namespace devmode::frame_editors
