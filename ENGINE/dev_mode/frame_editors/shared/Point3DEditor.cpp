#include "Point3DEditor.hpp"

#include <cmath>
#include <string>
#include "dev_mode/widgets.hpp"
#include "dev_mode/dm_styles.hpp"
#include "render/warped_screen_grid.hpp"
#include "world/grid_point.hpp"
#include "asset/Asset.hpp"

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
    tb_dx_ = std::make_unique<DMTextBox>("X (Left/Right)", "0");
    tb_dy_ = std::make_unique<DMTextBox>("Y (Depth)", "0");
    tb_dz_ = std::make_unique<DMTextBox>("Z (Height)", "0");
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

    // Return height needed for the three textboxes with extra padding
    return DMTextBox::height() + DMSpacing::small_gap() * 4;
}

void Point3DEditor::render_axis_point(SDL_Renderer* renderer,
                                    SDL_FPoint screen_pos,
                                    AdjustmentAxis axis,
                                    bool is_selected,
                                    float radius) {
    if (!renderer) return;

    SDL_Color point_color = get_axis_color(axis);
    const float outer_radius = radius * 1.5f;  // Larger circle
    const float center_radius = radius * 0.4f;  // Small center point

    // Draw larger outer circle with axis color
    SDL_SetRenderDrawColor(renderer, point_color.r, point_color.g, point_color.b, is_selected ? 255 : 180);
    for (float y = -outer_radius; y <= outer_radius; ++y) {
        for (float x = -outer_radius; x <= outer_radius; ++x) {
            float dist_sq = x * x + y * y;
            if (dist_sq <= outer_radius * outer_radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw white border around larger circle
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    const float border_inner = outer_radius;
    const float border_outer = outer_radius + 1.5f;
    for (float y = -border_outer; y <= border_outer; ++y) {
        for (float x = -border_outer; x <= border_outer; ++x) {
            float dist_sq = x * x + y * y;
            if (dist_sq >= border_inner * border_inner && dist_sq <= border_outer * border_outer) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw small center point (always on top, white)
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    for (float y = -center_radius; y <= center_radius; ++y) {
        for (float x = -center_radius; x <= center_radius; ++x) {
            if (x * x + y * y <= center_radius * center_radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw movement arrows if selected (longer and more pronounced)
    if (is_selected) {
        render_movement_arrows(renderer, screen_pos, axis, 32.0f);
    }
}

void Point3DEditor::render_axis_point_with_depth(SDL_Renderer* renderer,
                                                SDL_FPoint screen_pos,
                                                float world_z,
                                                AdjustmentAxis axis,
                                                bool is_selected,
                                                float radius) {
    if (!renderer) return;

    // Apply depth-based scaling for realism warping
    // Objects further back (lower Z) appear smaller
    // This simulates 3D perspective
    const float z_scale_factor = 1.0f + (world_z * 0.001f);  // Subtle scaling based on Z
    const float depth_scaled_radius = radius * std::max(0.5f, std::min(1.5f, z_scale_factor));

    SDL_Color point_color = get_axis_color(axis);
    const float outer_radius = depth_scaled_radius * 1.5f;  // Larger circle with depth scaling
    const float center_radius = depth_scaled_radius * 0.4f;  // Small center point with depth scaling

    // Apply subtle depth-based darkening for objects behind
    const int depth_darkness = static_cast<int>(std::max(0.0f, std::min(50.0f, -world_z * 0.05f)));

    // Draw larger outer circle with axis color and depth shading
    SDL_Color outer_color = point_color;
    outer_color.r = static_cast<Uint8>(std::max(0, outer_color.r - depth_darkness));
    outer_color.g = static_cast<Uint8>(std::max(0, outer_color.g - depth_darkness));
    outer_color.b = static_cast<Uint8>(std::max(0, outer_color.b - depth_darkness));
    SDL_SetRenderDrawColor(renderer, outer_color.r, outer_color.g, outer_color.b, is_selected ? 255 : 180);

    for (float y = -outer_radius; y <= outer_radius; ++y) {
        for (float x = -outer_radius; x <= outer_radius; ++x) {
            float dist_sq = x * x + y * y;
            if (dist_sq <= outer_radius * outer_radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw white border around larger circle
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    const float border_inner = outer_radius;
    const float border_outer = outer_radius + 1.5f;
    for (float y = -border_outer; y <= border_outer; ++y) {
        for (float x = -border_outer; x <= border_outer; ++x) {
            float dist_sq = x * x + y * y;
            if (dist_sq >= border_inner * border_inner && dist_sq <= border_outer * border_outer) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw small center point (always on top, white, no depth scaling)
    // This ensures the center is always clearly visible
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    const float fixed_center_radius = radius * 0.4f;  // Fixed size for center
    for (float y = -fixed_center_radius; y <= fixed_center_radius; ++y) {
        for (float x = -fixed_center_radius; x <= fixed_center_radius; ++x) {
            if (x * x + y * y <= fixed_center_radius * fixed_center_radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw movement arrows if selected (scaled with depth)
    if (is_selected) {
        const float arrow_length = 32.0f * std::max(0.7f, std::min(1.3f, z_scale_factor));
        render_movement_arrows(renderer, screen_pos, axis, arrow_length);
    }
}

void Point3DEditor::render_axis_point_with_camera(SDL_Renderer* renderer,
                                                  SDL_FPoint world_pos,
                                                  float world_z,
                                                  const void* camera_grid,
                                                  AdjustmentAxis axis,
                                                  bool is_selected,
                                                  float radius) {
#ifndef FRAME_EDITOR_TEST_PUBLIC_ACCESS
    if (!renderer || !camera_grid) return;

    const WarpedScreenGrid* cam = static_cast<const WarpedScreenGrid*>(camera_grid);

    // Project world point to screen using camera projection
    SDL_FPoint screen_pos;
    if (!cam->project_world_point(world_pos, world_z, screen_pos)) {
        // Fallback to simple map_to_screen if projection fails
        screen_pos = cam->map_to_screen_f(world_pos);
    }

    // Calculate perspective scale based on world Y (depth) position
    // In this engine, Y is depth (forward/back), Z is height
    const float depth_y = world_pos.y;
    const float height_z = world_z;

    // Get camera settings for realism warping
    const auto& settings = cam->get_settings();
    const float base_height = settings.base_height_px;
    const float depth_near = settings.depth_near_world;
    const float depth_far = settings.depth_far_world;

    // Calculate perspective scale based on depth
    // Objects further away (higher Y) should appear smaller
    float depth_ratio = 0.5f;
    if (depth_far > depth_near) {
        depth_ratio = std::clamp((depth_y - depth_near) / (depth_far - depth_near), 0.0f, 1.0f);
    }

    // Apply perspective scaling (further = smaller)
    // Objects at near plane are larger, at far plane are smaller
    const float perspective_scale = 1.0f + (1.0f - depth_ratio) * 0.5f - depth_ratio * 0.3f;
    const float camera_scaled_radius = radius * std::max(0.5f, std::min(2.0f, perspective_scale));

    // Also apply height-based darkening (lower Z = darker, as if in shadow)
    const int height_darkness = static_cast<int>(std::max(0.0f, std::min(40.0f, -height_z * 0.02f)));
    const int depth_darkness = static_cast<int>(std::max(0.0f, std::min(30.0f, depth_ratio * 30.0f)));
    const int total_darkness = height_darkness + depth_darkness;

    SDL_Color point_color = get_axis_color(axis);
    const float outer_radius = camera_scaled_radius * 1.5f;
    const float center_radius = camera_scaled_radius * 0.4f;

    // Apply depth and height shading
    SDL_Color outer_color = point_color;
    outer_color.r = static_cast<Uint8>(std::max(0, outer_color.r - total_darkness));
    outer_color.g = static_cast<Uint8>(std::max(0, outer_color.g - total_darkness));
    outer_color.b = static_cast<Uint8>(std::max(0, outer_color.b - total_darkness));
    SDL_SetRenderDrawColor(renderer, outer_color.r, outer_color.g, outer_color.b, is_selected ? 255 : 180);

    // Draw larger outer circle with camera perspective
    for (float y = -outer_radius; y <= outer_radius; ++y) {
        for (float x = -outer_radius; x <= outer_radius; ++x) {
            float dist_sq = x * x + y * y;
            if (dist_sq <= outer_radius * outer_radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw white border
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    const float border_inner = outer_radius;
    const float border_outer = outer_radius + 1.5f;
    for (float y = -border_outer; y <= border_outer; ++y) {
        for (float x = -border_outer; x <= border_outer; ++x) {
            float dist_sq = x * x + y * y;
            if (dist_sq >= border_inner * border_inner && dist_sq <= border_outer * border_outer) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw small center point (always visible, fixed size)
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    const float fixed_center_radius = radius * 0.4f;
    for (float y = -fixed_center_radius; y <= fixed_center_radius; ++y) {
        for (float x = -fixed_center_radius; x <= fixed_center_radius; ++x) {
            if (x * x + y * y <= fixed_center_radius * fixed_center_radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    // Draw movement arrows if selected
    if (is_selected) {
        const float arrow_length = 32.0f * perspective_scale;
        render_movement_arrows(renderer, screen_pos, axis, arrow_length);
    }
#else
    // Test stub: just use basic rendering without camera projection
    (void)camera_grid; // Suppress unused parameter warning
    render_axis_point_with_depth(renderer, world_pos, world_z, axis, is_selected, radius);
#endif
}

bool Point3DEditor::handle_point_click(const SDL_Point& mouse_pos,
                                     SDL_FPoint point_screen_pos,
                                     float radius) {
    const float dx = static_cast<float>(mouse_pos.x) - point_screen_pos.x;
    const float dy = static_cast<float>(mouse_pos.y) - point_screen_pos.y;
    const float outer_radius = radius * 1.5f;
    return (dx * dx + dy * dy) <= (outer_radius * outer_radius);
}

SDL_FPoint Point3DEditor::constrain_drag_delta(const SDL_FPoint& drag_delta,
                                             AdjustmentAxis axis) {
    SDL_FPoint constrained = drag_delta;
    switch (axis) {
        case AdjustmentAxis::X:
            constrained.y = 0.0f;  // Only horizontal movement (left/right)
            break;
        case AdjustmentAxis::Y:
            constrained.x = 0.0f;  // Y is depth (forward/back in screen vertical)
            break;
        case AdjustmentAxis::Z:
            constrained.x = 0.0f;  // Z is height (up/down in screen vertical)
            break;
    }
    return constrained;
}

SDL_Color Point3DEditor::get_axis_color(AdjustmentAxis axis) {
    switch (axis) {
        case AdjustmentAxis::X:
            return SDL_Color{255, 50, 50, 255};  // Red (left/right)
        case AdjustmentAxis::Y:
            return SDL_Color{50, 255, 50, 255};  // Green (depth - forward/back)
        case AdjustmentAxis::Z:
            return SDL_Color{50, 150, 255, 255};  // Blue (height - up/down)
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

    const float arrow_size = 6.0f;  // Larger arrow heads
    const float half_length = length * 0.5f;
    const float line_width = 2.0f;  // Make lines thicker

    // Draw thicker lines by drawing multiple parallel lines
    auto draw_thick_line = [&](float x1, float y1, float x2, float y2) {
        for (float offset = -line_width/2; offset <= line_width/2; offset += 0.5f) {
            if (std::fabs(x2 - x1) > std::fabs(y2 - y1)) {
                // More horizontal - offset vertically
                SDL_RenderDrawLineF(renderer, x1, y1 + offset, x2, y2 + offset);
            } else {
                // More vertical - offset horizontally
                SDL_RenderDrawLineF(renderer, x1 + offset, y1, x2 + offset, y2);
            }
        }
    };

    switch (axis) {
        case AdjustmentAxis::X: {
            // Horizontal arrows (left/right)
            // Main line
            draw_thick_line(center.x - half_length, center.y, center.x + half_length, center.y);

            // Left arrow
            SDL_RenderDrawLineF(renderer, center.x - half_length, center.y,
                             center.x - half_length + arrow_size, center.y - arrow_size);
            SDL_RenderDrawLineF(renderer, center.x - half_length, center.y,
                             center.x - half_length + arrow_size, center.y + arrow_size);

            // Right arrow
            SDL_RenderDrawLineF(renderer, center.x + half_length, center.y,
                             center.x + half_length - arrow_size, center.y - arrow_size);
            SDL_RenderDrawLineF(renderer, center.x + half_length, center.y,
                             center.x + half_length - arrow_size, center.y + arrow_size);
            break;
        }
        case AdjustmentAxis::Y:
        case AdjustmentAxis::Z: {
            // Vertical arrows (up/down) - Z uses same as Y since it moves vertically on screen
            // Main line
            draw_thick_line(center.x, center.y - half_length, center.x, center.y + half_length);

            // Up arrow
            SDL_RenderDrawLineF(renderer, center.x, center.y - half_length,
                             center.x - arrow_size, center.y - half_length + arrow_size);
            SDL_RenderDrawLineF(renderer, center.x, center.y - half_length,
                             center.x + arrow_size, center.y - half_length + arrow_size);

            // Down arrow
            SDL_RenderDrawLineF(renderer, center.x, center.y + half_length,
                             center.x - arrow_size, center.y + half_length - arrow_size);
            SDL_RenderDrawLineF(renderer, center.x, center.y + half_length,
                             center.x + arrow_size, center.y + half_length - arrow_size);
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
                const int clicked_index = static_cast<int>(i);

                // Check if clicking already-selected point
                if (clicked_index == selected_point_index_) {
                    // Second click on same point - cycle axis
                    cycle_axis();
                } else {
                    // First click on new point - select it but don't change axis
                    selected_point_index_ = clicked_index;
                    if (on_point_selected_) {
                        on_point_selected_(clicked_index);
                    }
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
    }

    // Scroll is NOT handled here anymore - reserved for camera height/tilt

    return is_dragging_;
}

}
