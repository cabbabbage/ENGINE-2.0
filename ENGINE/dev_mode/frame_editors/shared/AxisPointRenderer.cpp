#include "AxisPointRenderer.hpp"

#include <cmath>

#include "AxisAdjuster.hpp"

namespace devmode::frame_editors {

void AxisPointRenderer::render_axis_point(SDL_Renderer* renderer,
                                        SDL_FPoint screen_pos,
                                        AdjustmentAxis axis,
                                        bool is_selected,
                                        float radius) {
    if (!renderer) return;

    SDL_Color point_color = get_axis_color(axis);
    if (!is_selected) {
        point_color.a = 180;  // More transparent when not selected
    }

    // Draw main point
    SDL_SetRenderDrawColor(renderer, point_color.r, point_color.g, point_color.b, point_color.a);
    const float draw_radius = is_selected ? radius + 2.0f : radius;
    for (float y = -draw_radius; y <= draw_radius; ++y) {
        for (float x = -draw_radius; x <= draw_radius; ++x) {
            if (x * x + y * y <= draw_radius * draw_radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw border
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    const int border_radius = static_cast<int>(draw_radius) + 1;
    for (int y = -border_radius; y <= border_radius; ++y) {
        for (int x = -border_radius; x <= border_radius; ++x) {
            if (x * x + y * y >= draw_radius * draw_radius &&
                x * x + y * y <= (draw_radius + 1.0f) * (draw_radius + 1.0f)) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw movement arrows if selected
    if (is_selected) {
        render_movement_arrows(renderer, screen_pos, axis);
    }
}

bool AxisPointRenderer::handle_point_click(const SDL_Point& mouse_pos,
                                         SDL_FPoint point_screen_pos,
                                         float radius) {
    const float dx = static_cast<float>(mouse_pos.x) - point_screen_pos.x;
    const float dy = static_cast<float>(mouse_pos.y) - point_screen_pos.y;
    return (dx * dx + dy * dy) <= (radius * radius);
}

SDL_FPoint AxisPointRenderer::constrain_drag_delta(const SDL_FPoint& drag_delta,
                                                 AdjustmentAxis axis) {
    SDL_FPoint constrained = drag_delta;
    switch (axis) {
        case AdjustmentAxis::X:
            constrained.y = 0.0f;  // Only horizontal movement
            break;
        case AdjustmentAxis::Y:
            constrained.x = 0.0f;  // Only vertical movement
            break;
        case AdjustmentAxis::Z:
            constrained.x = 0.0f;  // Z axis uses vertical screen movement
            break;
    }
    return constrained;
}

void AxisPointRenderer::cycle_axis_on_selection(SelectionState* selection_state,
                                              AxisAdjuster* axis_adjuster) {
    if (axis_adjuster) {
        axis_adjuster->cycle_axis();
    }
}

SDL_Color AxisPointRenderer::get_axis_color(AdjustmentAxis axis) {
    switch (axis) {
        case AdjustmentAxis::X:
            return SDL_Color{255, 50, 50, 255};  // Red
        case AdjustmentAxis::Y:
            return SDL_Color{50, 255, 50, 255};  // Green
        case AdjustmentAxis::Z:
            return SDL_Color{50, 50, 255, 255};  // Blue
        default:
            return SDL_Color{128, 128, 128, 255};  // Gray fallback
    }
}

void AxisPointRenderer::render_movement_arrows(SDL_Renderer* renderer,
                                             SDL_FPoint center,
                                             AdjustmentAxis axis,
                                             float length) {
    if (!renderer) return;

    SDL_Color arrow_color = get_axis_color(axis);
    SDL_SetRenderDrawColor(renderer, arrow_color.r, arrow_color.g, arrow_color.b, 255);

    const float arrow_size = 4.0f;
    const float half_length = length * 0.5f;

    switch (axis) {
        case AdjustmentAxis::X: {
            // Horizontal arrows (left/right)
            // Left arrow
            SDL_RenderDrawLineF(renderer, center.x - half_length, center.y,
                             center.x - half_length + arrow_size, center.y - arrow_size);
            SDL_RenderDrawLineF(renderer, center.x - half_length, center.y,
                             center.x - half_length + arrow_size, center.y + arrow_size);
            SDL_RenderDrawLineF(renderer, center.x - half_length - arrow_size, center.y,
                             center.x - half_length, center.y);

            // Right arrow
            SDL_RenderDrawLineF(renderer, center.x + half_length, center.y,
                             center.x + half_length - arrow_size, center.y - arrow_size);
            SDL_RenderDrawLineF(renderer, center.x + half_length, center.y,
                             center.x + half_length - arrow_size, center.y + arrow_size);
            SDL_RenderDrawLineF(renderer, center.x + half_length + arrow_size, center.y,
                             center.x + half_length, center.y);
            break;
        }
        case AdjustmentAxis::Y:
        case AdjustmentAxis::Z: {
            // Vertical arrows (up/down) - Z uses same as Y since it moves vertically on screen
            // Up arrow
            SDL_RenderDrawLineF(renderer, center.x, center.y - half_length,
                             center.x - arrow_size, center.y - half_length + arrow_size);
            SDL_RenderDrawLineF(renderer, center.x, center.y - half_length,
                             center.x + arrow_size, center.y - half_length + arrow_size);
            SDL_RenderDrawLineF(renderer, center.x, center.y - half_length - arrow_size,
                             center.x, center.y - half_length);

            // Down arrow
            SDL_RenderDrawLineF(renderer, center.x, center.y + half_length,
                             center.x - arrow_size, center.y + half_length - arrow_size);
            SDL_RenderDrawLineF(renderer, center.x, center.y + half_length,
                             center.x + arrow_size, center.y + half_length - arrow_size);
            SDL_RenderDrawLineF(renderer, center.x, center.y + half_length + arrow_size,
                             center.x, center.y + half_length);
            break;
        }
    }
}

}  // namespace devmode::frame_editors
