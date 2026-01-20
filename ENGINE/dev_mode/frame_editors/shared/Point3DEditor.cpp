#include "Point3DEditor.hpp"

#include <cmath>
#include <string>
#include "dev_mode/widgets.hpp"
#include "dev_mode/dm_styles.hpp"

namespace devmode::frame_editors {

namespace {

float parse_float(const std::string& text, float fallback) {
    try {
        return std::stof(text);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

Point3DEditor::Point3DEditor(SelectionState* selection)
    : selection_(selection) {
    tb_dx_ = std::make_unique<DMTextBox>("dx", "0");
    tb_dy_ = std::make_unique<DMTextBox>("dy", "0");
    tb_dz_ = std::make_unique<DMTextBox>("dz", "0");
}

Point3DEditor::~Point3DEditor() = default;

void Point3DEditor::set_selection(SelectionState* selection) {
    selection_ = selection;
    sync_textboxes_from_selection();
}

void Point3DEditor::cycle_axis() {
    if (!selection_) return;
    switch (selection_->axis) {
        case AdjustmentAxis::X: selection_->axis = AdjustmentAxis::Y; break;
        case AdjustmentAxis::Y: selection_->axis = AdjustmentAxis::Z; break;
        case AdjustmentAxis::Z: selection_->axis = AdjustmentAxis::X; break;
    }
}

void Point3DEditor::reset_axis(AdjustmentAxis axis) {
    if (selection_) {
        selection_->axis = axis;
    }
}

void Point3DEditor::set_axis_from_textbox_click(int textbox_index) {
    AdjustmentAxis new_axis = AdjustmentAxis::X;
    switch (textbox_index) {
        case 0: new_axis = AdjustmentAxis::X; break; // dx
        case 1: new_axis = AdjustmentAxis::Y; break; // dy
        case 2: new_axis = AdjustmentAxis::Z; break; // dz
    }
    reset_axis(new_axis);
}

bool Point3DEditor::handle_event(const SDL_Event& e, const SDL_Rect& container) {
    if (!selection_ || !selection_->has_target()) {
        return false;
    }

    // Check for textbox clicks to set axis
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point mouse_pos = {e.button.x, e.button.y};
        const int padding = DMSpacing::small_gap();
        const int inner_w = container.w - padding * 2;
        const int third_w = (inner_w - DMSpacing::small_gap() * 2) / 3;
        int y = container.y + padding;

        if (tb_dx_) {
            SDL_Rect dx_rect{
                container.x + padding,
                y,
                third_w,
                tb_dx_->height_for_width(third_w)
            };
            if (SDL_PointInRect(&mouse_pos, &dx_rect)) {
                set_axis_from_textbox_click(0);
            }
        }

        if (tb_dy_) {
            SDL_Rect dy_rect{
                container.x + padding + third_w + DMSpacing::small_gap(),
                y,
                third_w,
                tb_dy_->height_for_width(third_w)
            };
            if (SDL_PointInRect(&mouse_pos, &dy_rect)) {
                set_axis_from_textbox_click(1);
            }
        }

        if (tb_dz_) {
            SDL_Rect dz_rect{
                container.x + padding + (third_w + DMSpacing::small_gap()) * 2,
                y,
                third_w,
                tb_dz_->height_for_width(third_w)
            };
            if (SDL_PointInRect(&mouse_pos, &dz_rect)) {
                set_axis_from_textbox_click(2);
            }
        }
    }

    bool consumed = false;

    if (tb_dx_ && tb_dx_->handle_event(e)) {
        apply_textbox_changes();
        consumed = true;
    }
    if (tb_dy_ && tb_dy_->handle_event(e)) {
        apply_textbox_changes();
        consumed = true;
    }
    if (tb_dz_ && tb_dz_->handle_event(e)) {
        apply_textbox_changes();
        consumed = true;
    }

    return consumed;
}

void Point3DEditor::render_overlays(SDL_Renderer* renderer, const SDL_Rect& container) {
    if (!renderer || !selection_ || !selection_->has_target()) {
        return;
    }

    sync_textboxes_from_selection();

    const int padding = DMSpacing::small_gap();
    const int inner_w = container.w - padding * 2;
    const int third_w = (inner_w - DMSpacing::small_gap() * 2) / 3;
    
    int y = container.y + padding;
    
    if (tb_dx_ && tb_dy_ && tb_dz_) {
        SDL_Rect dx_rect{
            container.x + padding,
            y,
            third_w,
            tb_dx_->height_for_width(third_w)
        };
        
        SDL_Rect dy_rect{
            container.x + padding + third_w + DMSpacing::small_gap(),
            y,
            third_w,
            tb_dy_->height_for_width(third_w)
        };
        
        SDL_Rect dz_rect{
            container.x + padding + (third_w + DMSpacing::small_gap()) * 2,
            y,
            third_w,
            tb_dz_->height_for_width(third_w)
        };
        
        tb_dx_->set_rect(dx_rect);
        tb_dy_->set_rect(dy_rect);
        tb_dz_->set_rect(dz_rect);
        
        tb_dx_->render(renderer);
        tb_dy_->render(renderer);
        tb_dz_->render(renderer);
    }
}

void Point3DEditor::sync_textboxes_from_selection() {
    if (!selection_ || !selection_->has_target()) {
        return;
    }

    // Only update if not currently editing
    if (tb_dx_ && !tb_dx_->is_editing()) {
        const std::string dx_str = std::to_string(static_cast<int>(std::lround(selection_->world_pos.x)));
        if (dx_str != last_dx_text_) {
            tb_dx_->set_value(dx_str);
            last_dx_text_ = dx_str;
        }
    }
    
    if (tb_dy_ && !tb_dy_->is_editing()) {
        const std::string dy_str = std::to_string(static_cast<int>(std::lround(selection_->world_pos.y)));
        if (dy_str != last_dy_text_) {
            tb_dy_->set_value(dy_str);
            last_dy_text_ = dy_str;
        }
    }
    
    if (tb_dz_ && !tb_dz_->is_editing()) {
        const std::string dz_str = std::to_string(static_cast<int>(std::lround(selection_->world_z)));
        if (dz_str != last_dz_text_) {
            tb_dz_->set_value(dz_str);
            last_dz_text_ = dz_str;
        }
    }
}

void Point3DEditor::apply_textbox_changes() {
    if (!selection_ || !selection_->has_target()) {
        return;
    }

    bool changed = false;
    
    if (tb_dx_) {
        const float value = parse_float(tb_dx_->value(), selection_->world_pos.x);
        if (std::fabs(value - selection_->world_pos.x) > 0.001f) {
            selection_->world_pos.x = value;
            changed = true;
            last_dx_text_ = tb_dx_->value();
        }
    }
    
    if (tb_dy_) {
        const float value = parse_float(tb_dy_->value(), selection_->world_pos.y);
        if (std::fabs(value - selection_->world_pos.y) > 0.001f) {
            selection_->world_pos.y = value;
            changed = true;
            last_dy_text_ = tb_dy_->value();
        }
    }
    
    if (tb_dz_) {
        const float value = parse_float(tb_dz_->value(), selection_->world_z);
        if (std::fabs(value - selection_->world_z) > 0.001f) {
            selection_->world_z = value;
            changed = true;
            last_dz_text_ = tb_dz_->value();
        }
    }

    if (changed && on_coordinates_changed_) {
        on_coordinates_changed_();
    }
}

int Point3DEditor::get_overlay_height() const {
    if (!selection_ || !selection_->has_target()) {
        return 0;
    }
    
    // Return height needed for the three textboxes
    return DMTextBox::height() + DMSpacing::small_gap() * 2;
}

void Point3DEditor::render_axis_point(SDL_Renderer* renderer,
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

bool Point3DEditor::handle_point_click(const SDL_Point& mouse_pos,
                                     SDL_FPoint point_screen_pos,
                                     float radius) {
    const float dx = static_cast<float>(mouse_pos.x) - point_screen_pos.x;
    const float dy = static_cast<float>(mouse_pos.y) - point_screen_pos.y;
    return (dx * dx + dy * dy) <= (radius * radius);
}

SDL_FPoint Point3DEditor::constrain_drag_delta(const SDL_FPoint& drag_delta,
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

SDL_Color Point3DEditor::get_axis_color(AdjustmentAxis axis) {
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

void Point3DEditor::render_movement_arrows(SDL_Renderer* renderer,
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

bool Point3DEditor::handle_mouse_event(const SDL_Event& e,
                                      const std::vector<SDL_FPoint>& point_screens,
                                      std::function<SDL_FPoint(const SDL_Point&)> screen_to_world) {
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point mouse_pos = {e.button.x, e.button.y};
        
        // Check for point selection
        for (std::size_t i = 0; i < point_screens.size(); ++i) {
            if (handle_point_click(mouse_pos, point_screens[i])) {
                if (on_point_selected_) {
                    on_point_selected_(static_cast<int>(i));
                }
                
                // Start dragging
                drag_start_mouse_pos_ = mouse_pos;
                drag_start_world_pos_ = screen_to_world(drag_start_mouse_pos_);
                if (selection_) {
                    drag_start_world_z_ = selection_->world_z;
                }
                is_dragging_ = true;
                return true;
            }
        }

        // If we were already dragging and clicked elsewhere, stop dragging
        if (is_dragging_) {
            is_dragging_ = false;
        }
    } else if (e.type == SDL_MOUSEMOTION && is_dragging_) {
        SDL_Point current_mouse = {e.motion.x, e.motion.y};
        SDL_FPoint screen_delta = {static_cast<float>(current_mouse.x - drag_start_mouse_pos_.x), 
                                  static_cast<float>(current_mouse.y - drag_start_mouse_pos_.y)};
        
        SDL_FPoint delta;
        if (selection_ && selection_->axis == AdjustmentAxis::Z) {
            delta = {0.0f, screen_delta.y};
        } else {
            SDL_FPoint current_world = screen_to_world(current_mouse);
            delta = {current_world.x - drag_start_world_pos_.x, current_world.y - drag_start_world_pos_.y};
        }
        
        SDL_FPoint constrained_delta = constrain_drag_delta(delta, axis());
        
        if (selection_ && selection_->axis == AdjustmentAxis::Z) {
            float new_world_z = drag_start_world_z_ - constrained_delta.y; // Invert screen Y for world Z
            if (on_position_changed_) {
                on_position_changed_(drag_start_world_pos_, new_world_z);
            }
        } else {
            SDL_FPoint new_world_pos = {drag_start_world_pos_.x + constrained_delta.x, 
                                       drag_start_world_pos_.y + constrained_delta.y};
            if (on_position_changed_) {
                on_position_changed_(new_world_pos, drag_start_world_z_);
            }
        }
        return true;
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && is_dragging_) {
        is_dragging_ = false;
        return true;
    } else if (e.type == SDL_MOUSEWHEEL) {
        if (selection_ && selection_->has_target()) {
            float precise = e.wheel.preciseY;
            int delta = e.wheel.y;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                delta = -delta;
                precise = -precise;
            }
            int steps = (std::fabs(precise) >= 0.01f) ? static_cast<int>(std::lround(precise)) : delta;
            if (steps != 0) {
                SDL_FPoint new_pos = selection_->world_pos;
                float new_z = selection_->world_z;
                switch (selection_->axis) {
                    case AdjustmentAxis::X:
                        new_pos.x += static_cast<float>(steps);
                        break;
                    case AdjustmentAxis::Y:
                        new_pos.y += static_cast<float>(steps);
                        break;
                    case AdjustmentAxis::Z:
                        new_z += static_cast<float>(steps);
                        break;
                }
                if (on_position_changed_) {
                    on_position_changed_(new_pos, new_z);
                }
                return true;
            }
        }
    }
    
    return is_dragging_;
}

}
