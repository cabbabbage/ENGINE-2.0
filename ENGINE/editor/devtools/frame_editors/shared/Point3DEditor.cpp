#include "Point3DEditor.hpp"
#include "utils/sdl_mouse_utils.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include "devtools/widgets.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "gameplay/world/grid_point.hpp"
#include "utils/grid.hpp"
#include "assets/asset/Asset.hpp"
#include "core/axis_convention.hpp"

namespace devmode::frame_editors {

namespace {

float parse_float(const std::string& text, float fallback) {
    try {
        return std::stof(text);
    } catch (...) {
        return fallback;
    }
}

struct AxisTextboxLayout {
    SDL_Rect panel_rect{0, 0, 0, 0};
    SDL_Rect dx_rect{0, 0, 0, 0};
    SDL_Rect dy_rect{0, 0, 0, 0};
    SDL_Rect dz_rect{0, 0, 0, 0};
    int total_height = 0;
};

AxisTextboxLayout compute_axis_textbox_layout(const SDL_Rect& container,
                                              const DMTextBox* tb_dx,
                                              const DMTextBox* tb_dy,
                                              const DMTextBox* tb_dz) {
    constexpr int kOuterBottomGap = 10;
    constexpr int kPanelInnerPadding = 10;
    constexpr int kTextboxGap = 10;
    constexpr int kTextboxMinWidth = 120;
    constexpr int kTextboxMaxWidth = 240;

    AxisTextboxLayout layout{};
    if (container.w <= 0 || container.h <= 0) {
        return layout;
    }

    const int side_margin = std::max(DMSpacing::item_gap(), 12);
    const int max_panel_w = std::max(0, container.w - side_margin * 2);
    int panel_w = max_panel_w;
    if (panel_w <= 0) {
        return layout;
    }

    const int preferred_three_col_w = kTextboxMaxWidth * 3 + kTextboxGap * 2 + kPanelInnerPadding * 2;
    panel_w = std::min(panel_w, preferred_three_col_w);
    if (panel_w <= 0) {
        return layout;
    }

    const int panel_x = container.x + (container.w - panel_w) / 2;
    const int content_w = std::max(1, panel_w - kPanelInnerPadding * 2);
    const bool can_fit_three = content_w >= (kTextboxMinWidth * 3 + kTextboxGap * 2);
    const bool can_fit_two = content_w >= (kTextboxMinWidth * 2 + kTextboxGap);

    int dx_w = content_w;
    int dy_w = content_w;
    int dz_w = content_w;
    if (can_fit_three) {
        const int col_w = std::max(kTextboxMinWidth, (content_w - kTextboxGap * 2) / 3);
        dx_w = col_w;
        dy_w = col_w;
        dz_w = col_w;
    } else if (can_fit_two) {
        const int col_w = std::max(kTextboxMinWidth, (content_w - kTextboxGap) / 2);
        dx_w = col_w;
        dy_w = col_w;
        dz_w = content_w;
    }

    const int dx_h = tb_dx ? tb_dx->height_for_width(dx_w) : DMTextBox::height();
    const int dy_h = tb_dy ? tb_dy->height_for_width(dy_w) : DMTextBox::height();
    const int dz_h = tb_dz ? tb_dz->height_for_width(dz_w) : DMTextBox::height();

    int panel_h = 0;
    if (can_fit_three) {
        panel_h = kPanelInnerPadding * 2 + std::max({dx_h, dy_h, dz_h});
    } else if (can_fit_two) {
        panel_h = kPanelInnerPadding * 2 + std::max(dx_h, dy_h) + kTextboxGap + dz_h;
    } else {
        panel_h = kPanelInnerPadding * 2 + dx_h + kTextboxGap + dy_h + kTextboxGap + dz_h;
    }

    const int panel_y = container.y + container.h - panel_h - kOuterBottomGap;
    layout.panel_rect = SDL_Rect{panel_x, panel_y, panel_w, panel_h};
    layout.total_height = panel_h + kOuterBottomGap * 2;

    const int content_x = panel_x + kPanelInnerPadding;
    const int content_y = panel_y + kPanelInnerPadding;

    if (can_fit_three) {
        const int row_h = std::max({dx_h, dy_h, dz_h});
        layout.dx_rect = SDL_Rect{content_x, content_y, dx_w, row_h};
        layout.dy_rect = SDL_Rect{content_x + dx_w + kTextboxGap, content_y, dy_w, row_h};
        layout.dz_rect = SDL_Rect{content_x + dx_w + kTextboxGap + dy_w + kTextboxGap, content_y, dz_w, row_h};
    } else if (can_fit_two) {
        const int top_h = std::max(dx_h, dy_h);
        layout.dx_rect = SDL_Rect{content_x, content_y, dx_w, top_h};
        layout.dy_rect = SDL_Rect{content_x + dx_w + kTextboxGap, content_y, dy_w, top_h};
        layout.dz_rect = SDL_Rect{content_x, content_y + top_h + kTextboxGap, dz_w, dz_h};
    } else {
        layout.dx_rect = SDL_Rect{content_x, content_y, dx_w, dx_h};
        layout.dy_rect = SDL_Rect{content_x, content_y + dx_h + kTextboxGap, dy_w, dy_h};
        layout.dz_rect = SDL_Rect{content_x, content_y + dx_h + kTextboxGap + dy_h + kTextboxGap, dz_w, dz_h};
    }

    return layout;
}

}  // namespace

Point3DEditor::Point3DEditor(SelectionState* selection)
    : selection_(selection) {
    tb_dx_ = std::make_unique<DMTextBox>("X", "0");
    tb_dy_ = std::make_unique<DMTextBox>("Y", "0");
    tb_dz_ = std::make_unique<DMTextBox>("Z", "0");
}

Point3DEditor::~Point3DEditor() = default;

void Point3DEditor::set_selection(SelectionState* selection) {
    selection_ = selection;
    sync_textboxes_from_selection();
}

void Point3DEditor::cycle_axis() {
    if (!selection_) return;
    selection_->axis = next_enabled_axis(selection_->axis);
}

void Point3DEditor::reset_axis(AdjustmentAxis axis) {
    if (selection_) {
        // If requested axis is disabled, fall back to first enabled axis
        selection_->axis = is_axis_enabled(axis) ? axis : first_enabled_axis();
    }
}

void Point3DEditor::set_axis_from_textbox_click(int textbox_index) {
    AdjustmentAxis new_axis = AdjustmentAxis::X;
    switch (textbox_index) {
        case 0: new_axis = AdjustmentAxis::X; break; // dx
        case 1: new_axis = AdjustmentAxis::Y; break; // dy
        case 2: new_axis = AdjustmentAxis::Z; break; // dz
    }
    if (is_axis_enabled(new_axis)) {
        reset_axis(new_axis);
    }
}

void Point3DEditor::set_grid_resolution(int resolution) {
    const int clamped = vibble::grid::clamp_resolution(std::max(0, resolution));
    grid_resolution_ = clamped;
    const int step = vibble::grid::delta(clamped);
    grid_step_world_ = static_cast<float>(step > 0 ? step : 1);
}

void Point3DEditor::set_parent_height(float height_px) {
    parent_height_px_ = (height_px > 0.0f && std::isfinite(height_px)) ? height_px : 0.0f;
}

void Point3DEditor::set_axis_enabled(AdjustmentAxis axis, bool enabled) {
    axis_enabled_[axis_to_index(axis)] = enabled;
    // If the current axis becomes disabled, move to the first enabled axis
    if (selection_ && !is_axis_enabled(selection_->axis)) {
        selection_->axis = first_enabled_axis();
    }
}

void Point3DEditor::set_axis_locked_value(AdjustmentAxis axis, std::optional<float> locked_value) {
    axis_locked_values_[axis_to_index(axis)] = locked_value;
}

bool Point3DEditor::is_axis_enabled(AdjustmentAxis axis) const {
    return axis_enabled_[axis_to_index(axis)];
}

int Point3DEditor::axis_to_index(AdjustmentAxis axis) const {
    switch (axis) {
        case AdjustmentAxis::X: return 0;
        case AdjustmentAxis::Y: return 1;
        case AdjustmentAxis::Z: return 2;
    }
    return 0;
}

AdjustmentAxis Point3DEditor::first_enabled_axis() const {
    for (int i = 0; i < 3; ++i) {
        if (axis_enabled_[i]) {
            return static_cast<AdjustmentAxis>(i);
        }
    }
    return AdjustmentAxis::X;
}

AdjustmentAxis Point3DEditor::next_enabled_axis(AdjustmentAxis current) const {
    int idx = axis_to_index(current);
    for (int step = 1; step <= 3; ++step) {
        const int next_idx = (idx + step) % 3;
        if (axis_enabled_[next_idx]) {
            return static_cast<AdjustmentAxis>(next_idx);
        }
    }
    return current;
}

bool Point3DEditor::handle_event(const SDL_Event& e, const SDL_Rect& container) {
    if (!selection_ || !selection_->has_target()) {
        return false;
    }

    // Use cached container if passed container is invalid (zero dimensions)
    // This happens when handle_event is called without renderer access
    const SDL_Rect& effective_container = (container.w > 0 && container.h > 0)
        ? container
        : cached_container_;

    const bool dx_locked = axis_locked_values_[axis_to_index(AdjustmentAxis::X)].has_value();
    const bool dy_locked = axis_locked_values_[axis_to_index(AdjustmentAxis::Y)].has_value();
    const bool dz_locked = axis_locked_values_[axis_to_index(AdjustmentAxis::Z)].has_value();

    const AxisTextboxLayout layout = compute_axis_textbox_layout(effective_container, tb_dx_.get(), tb_dy_.get(), tb_dz_.get());
    const SDL_Rect dx_rect = layout.dx_rect;
    const SDL_Rect dy_rect = layout.dy_rect;
    const SDL_Rect dz_rect = layout.dz_rect;

    bool consumed = false;
    bool pointer_clicked_textbox = false;

    if (tb_dx_ && is_axis_enabled(AdjustmentAxis::X) && !dx_locked) {
        tb_dx_->set_rect(dx_rect);
    }
    if (tb_dy_ && is_axis_enabled(AdjustmentAxis::Y) && !dy_locked) {
        tb_dy_->set_rect(dy_rect);
    }
    if (tb_dz_ && is_axis_enabled(AdjustmentAxis::Z) && !dz_locked) {
        tb_dz_->set_rect(dz_rect);
    }

    if (tb_dx_ && is_axis_enabled(AdjustmentAxis::X) && !dx_locked && tb_dx_->handle_event(e)) {
        apply_textbox_changes();
        consumed = true;
    }
    if (tb_dy_ && is_axis_enabled(AdjustmentAxis::Y) && !dy_locked && tb_dy_->handle_event(e)) {
        apply_textbox_changes();
        consumed = true;
    }
    if (tb_dz_ && is_axis_enabled(AdjustmentAxis::Z) && !dz_locked && tb_dz_->handle_event(e)) {
        apply_textbox_changes();
        consumed = true;
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point mouse_pos = sdl_mouse_util::ButtonPoint(e.button);
        if (SDL_PointInRect(&mouse_pos, &dx_rect) && is_axis_enabled(AdjustmentAxis::X)) {
            set_axis_from_textbox_click(0);
            pointer_clicked_textbox = true;
        } else if (SDL_PointInRect(&mouse_pos, &dy_rect) && is_axis_enabled(AdjustmentAxis::Y)) {
            set_axis_from_textbox_click(1);
            pointer_clicked_textbox = true;
        } else if (SDL_PointInRect(&mouse_pos, &dz_rect) && is_axis_enabled(AdjustmentAxis::Z)) {
            set_axis_from_textbox_click(2);
            pointer_clicked_textbox = true;
        }
    }

    if (pointer_clicked_textbox) {
        consumed = true;
    }

    return consumed;
}

void Point3DEditor::render_overlays(SDL_Renderer* renderer, const SDL_Rect& container) {
    if (!renderer || !selection_ || !selection_->has_target()) {
        return;
    }

    // Cache the container rect for use in handle_event
    cached_container_ = container;

    sync_textboxes_from_selection();

    const AxisTextboxLayout layout = compute_axis_textbox_layout(container, tb_dx_.get(), tb_dy_.get(), tb_dz_.get());
    if (layout.panel_rect.w <= 0 || layout.panel_rect.h <= 0) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect(renderer,
                             layout.panel_rect,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity() * 0.45f,
                             DMStyles::ShadowIntensity() * 0.6f);
    dm_draw::DrawRoundedOutline(renderer,
                                layout.panel_rect,
                                DMStyles::CornerRadius(),
                                1,
                                DMStyles::Border());

    if (tb_dx_ && tb_dy_ && tb_dz_) {
        tb_dx_->set_rect(layout.dx_rect);
        tb_dy_->set_rect(layout.dy_rect);
        tb_dz_->set_rect(layout.dz_rect);

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
    const SDL_FPoint rel_pos = selection_->relative_world_pos();
    const float rel_z = selection_->relative_world_z();

    const auto locked_x = axis_locked_values_[axis_to_index(AdjustmentAxis::X)];
    const auto locked_y = axis_locked_values_[axis_to_index(AdjustmentAxis::Y)];
    const auto locked_z = axis_locked_values_[axis_to_index(AdjustmentAxis::Z)];

    if (tb_dx_) {
        std::string dx_str;
        if (xy_display_mode_ == CoordinateDisplayMode::RawDelta) {
            // Display X as raw integer value
            const float display_x = locked_x.value_or(rel_pos.x);
            dx_str = std::to_string(static_cast<int>(std::lround(display_x)));
        } else {
            // Display X as a percentage (0.0-1.0) of parent height
            float display_x_percent = 0.0f;
            if (parent_height_px_ > 0.0f) {
                const float world_x_offset = locked_x.value_or(rel_pos.x);
                display_x_percent = world_x_offset / parent_height_px_;
                display_x_percent = std::clamp(display_x_percent, 0.0f, 1.0f);
            }
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << display_x_percent;
            dx_str = oss.str();
        }
        if (!tb_dx_->is_editing() && dx_str != last_dx_text_) {
            tb_dx_->set_value(dx_str);
            last_dx_text_ = dx_str;
        }
        if (!is_axis_enabled(AdjustmentAxis::X)) {
            tb_dx_->set_label_color_override(SDL_Color{160, 160, 160, 255});
        } else {
            tb_dx_->clear_label_color_override();
        }
    }

    if (tb_dy_) {
        std::string dy_str;
        if (xy_display_mode_ == CoordinateDisplayMode::RawDelta) {
            // Display Y as raw integer value
            const float display_y = locked_y.value_or(rel_pos.y);
            dy_str = std::to_string(static_cast<int>(std::lround(display_y)));
        } else {
            // Display Y as a percentage (0.0-1.0) of parent height
            float display_y_percent = 0.0f;
            if (parent_height_px_ > 0.0f) {
                const float world_y_offset = locked_y.value_or(rel_pos.y);
                display_y_percent = world_y_offset / parent_height_px_;
                display_y_percent = std::clamp(display_y_percent, 0.0f, 1.0f);
            }
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << display_y_percent;
            dy_str = oss.str();
        }
        if (!tb_dy_->is_editing() && dy_str != last_dy_text_) {
            tb_dy_->set_value(dy_str);
            last_dy_text_ = dy_str;
        }
        if (!is_axis_enabled(AdjustmentAxis::Y)) {
            tb_dy_->set_label_color_override(SDL_Color{160, 160, 160, 255});
        } else {
            tb_dy_->clear_label_color_override();
        }
    }

    if (tb_dz_) {
        std::string dz_str;
        if (z_display_mode_ == CoordinateDisplayMode::RawDelta) {
            // Display Z as raw integer value (like dx/dy)
            const float display_z = locked_z.value_or(rel_z);
            dz_str = std::to_string(static_cast<int>(std::lround(display_z)));
        } else {
            // Display Z as a percentage (0.0-1.0) of parent height
            float display_z_percent = 0.0f;
            if (parent_height_px_ > 0.0f) {
                const float world_z_offset = locked_z.value_or(rel_z);
                display_z_percent = world_z_offset / parent_height_px_;
                display_z_percent = std::clamp(display_z_percent, 0.0f, 1.0f);
            }
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << display_z_percent;
            dz_str = oss.str();
        }
        if (!tb_dz_->is_editing() && dz_str != last_dz_text_) {
            tb_dz_->set_value(dz_str);
            last_dz_text_ = dz_str;
        }
        if (!is_axis_enabled(AdjustmentAxis::Z)) {
            tb_dz_->set_label_color_override(SDL_Color{160, 160, 160, 255});
        } else {
            tb_dz_->clear_label_color_override();
        }
    }
}

void Point3DEditor::apply_textbox_changes() {
    if (!selection_ || !selection_->has_target()) {
        return;
    }

    bool changed = false;
    const SDL_FPoint anchor = selection_->anchor_point();
    const SDL_FPoint rel_pos = selection_->relative_world_pos();
    const float rel_z = selection_->relative_world_z();
    const float anchor_z = selection_->anchor_z_point();

    const auto locked_x = axis_locked_values_[axis_to_index(AdjustmentAxis::X)];
    const auto locked_y = axis_locked_values_[axis_to_index(AdjustmentAxis::Y)];
    const auto locked_z = axis_locked_values_[axis_to_index(AdjustmentAxis::Z)];

    if (tb_dx_ && is_axis_enabled(AdjustmentAxis::X)) {
        const float value = locked_x.value_or(parse_float(tb_dx_->value(), rel_pos.x));
        const float new_world_x = anchor.x + value;
        if (std::fabs(new_world_x - selection_->world_pos.x) > 0.001f) {
            selection_->world_pos.x = new_world_x;
            changed = true;
            last_dx_text_ = tb_dx_->value();
        }
    }

    if (tb_dy_ && is_axis_enabled(AdjustmentAxis::Y)) {
        const float value = locked_y.value_or(parse_float(tb_dy_->value(), rel_pos.y));
        const float new_world_y = anchor.y + value;
        if (std::fabs(new_world_y - selection_->world_pos.y) > 0.001f) {
            selection_->world_pos.y = new_world_y;
            changed = true;
            last_dy_text_ = tb_dy_->value();
        }
    }

    if (tb_dz_ && is_axis_enabled(AdjustmentAxis::Z)) {
        float new_world_z;
        if (z_display_mode_ == CoordinateDisplayMode::RawDelta) {
            // Textbox contains a raw delta value (like dx/dy)
            const float value = locked_z.value_or(parse_float(tb_dz_->value(), rel_z));
            new_world_z = anchor_z + value;
        } else {
            // Textbox contains a percent value (0.0-1.0)
            // Convert to world Z: anchor_z + (percent * parent_height_px_)
            float current_percent = 0.0f;
            if (parent_height_px_ > 0.0f) {
                current_percent = rel_z / parent_height_px_;
            }
            const float input_percent = locked_z.has_value()
                ? (parent_height_px_ > 0.0f ? locked_z.value() / parent_height_px_ : 0.0f)
                : parse_float(tb_dz_->value(), current_percent);
            const float clamped_percent = std::clamp(input_percent, 0.0f, 1.0f);
            new_world_z = anchor_z + (clamped_percent * parent_height_px_);
        }
        if (std::fabs(new_world_z - selection_->world_z) > 0.001f) {
            selection_->world_z = new_world_z;
            changed = true;
            last_dz_text_ = tb_dz_->value();
        }
    }

    if (changed && on_coordinates_changed_) {
        on_coordinates_changed_();
    }
}

int Point3DEditor::get_overlay_height(int container_width) const {
    const SDL_Rect probe_rect{0, 0, std::max(0, container_width), 2000};
    const AxisTextboxLayout layout = compute_axis_textbox_layout(probe_rect, tb_dx_.get(), tb_dy_.get(), tb_dz_.get());
    return std::max(DMTextBox::height(), layout.total_height);
}

void Point3DEditor::render_axis_point(SDL_Renderer* renderer,
                                    SDL_FPoint screen_pos,
                                    AdjustmentAxis axis,
                                    bool is_selected,
                                    float radius) {
    if (!renderer) return;

    SDL_Color point_color = get_axis_color(axis);
    const float center_radius = radius * 0.4f;  // Small center point

    // Only draw larger outer circle if selected
    if (is_selected) {
        render_axis_line(renderer, screen_pos, axis, 180.0f);

        const float outer_radius = radius * 1.5f;  // Larger circle

        // Draw larger outer circle with axis color
        SDL_SetRenderDrawColor(renderer, point_color.r, point_color.g, point_color.b, 255);
        for (float y = -outer_radius; y <= outer_radius; ++y) {
            for (float x = -outer_radius; x <= outer_radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= outer_radius * outer_radius) {
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
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
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }

    }

    // Always draw small center point (always on top, white)
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    for (float y = -center_radius; y <= center_radius; ++y) {
        for (float x = -center_radius; x <= center_radius; ++x) {
            if (x * x + y * y <= center_radius * center_radius) {
                SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
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
    const float center_radius = depth_scaled_radius * 0.4f;  // Small center point with depth scaling

    // Only draw larger outer circle if selected
    if (is_selected) {
        SDL_Color line_color = point_color;
        line_color.a = 140;
        const float length = 180.0f * std::max(0.5f, std::min(1.5f, std::max(0.5f, std::min(1.5f, 1.0f + (world_z * 0.001f)))));
        render_axis_line(renderer, screen_pos, axis, length, line_color.a);

        const float outer_radius = depth_scaled_radius * 1.5f;  // Larger circle with depth scaling

        // Apply subtle depth-based darkening for objects behind
        const int depth_darkness = static_cast<int>(std::max(0.0f, std::min(50.0f, -world_z * 0.05f)));

        // Draw larger outer circle with axis color and depth shading
        SDL_Color outer_color = point_color;
        outer_color.r = static_cast<Uint8>(std::max(0, outer_color.r - depth_darkness));
        outer_color.g = static_cast<Uint8>(std::max(0, outer_color.g - depth_darkness));
        outer_color.b = static_cast<Uint8>(std::max(0, outer_color.b - depth_darkness));
        SDL_SetRenderDrawColor(renderer, outer_color.r, outer_color.g, outer_color.b, 255);

        for (float y = -outer_radius; y <= outer_radius; ++y) {
            for (float x = -outer_radius; x <= outer_radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= outer_radius * outer_radius) {
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
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
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }

        // Draw movement arrows if selected (scaled with depth)
        const float arrow_length = 32.0f * std::max(0.7f, std::min(1.3f, z_scale_factor));
        render_movement_arrows(renderer, screen_pos, axis, arrow_length);
    }

    // Always draw small center point (always on top, white, no depth scaling)
    // This ensures the center is always clearly visible
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    const float fixed_center_radius = radius * 0.4f;  // Fixed size for center
    for (float y = -fixed_center_radius; y <= fixed_center_radius; ++y) {
        for (float x = -fixed_center_radius; x <= fixed_center_radius; ++x) {
            if (x * x + y * y <= fixed_center_radius * fixed_center_radius) {
                SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
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

    // Calculate perspective scale based on depth (Z) and height (Y).
    // world_pos.y is the actual height axis; world_z is forward/back depth.
    const float height_y = world_pos.y;
    const float depth_z = world_z;

    // Calculate perspective scaling from true camera-space forward depth.
    const world::CameraProjectionParams projection = cam->projection_params();
    const double meters_scale = std::max(1e-6, projection.meters_scale);
    const double world_m_x = (static_cast<double>(world_pos.x) - projection.anchor_world_x) * meters_scale;
    const double world_m_y = (static_cast<double>(world_pos.y) - projection.anchor_world_y) * meters_scale;
    const double world_m_z = (static_cast<double>(depth_z) - projection.anchor_world_z) * meters_scale;
    const double to_x = world_m_x - projection.position_x;
    const double to_y = world_m_y - projection.position_y;
    const double to_z = world_m_z - projection.position_z;
    const double forward_depth =
        to_x * projection.forward_x + to_y * projection.forward_y + to_z * projection.forward_z;

    float depth_ratio = 0.5f;
    const double near_depth = std::max(1e-4, projection.near_plane);
    const double far_depth = std::max(near_depth + 1e-4, projection.far_plane);
    if (std::isfinite(forward_depth)) {
        depth_ratio = std::clamp(
            static_cast<float>((forward_depth - near_depth) / (far_depth - near_depth)),
            0.0f,
            1.0f);
    }

    // Apply perspective scaling (further = smaller)
    // Objects at near plane are larger, at far plane are smaller
    const float perspective_scale = 1.0f + (1.0f - depth_ratio) * 0.5f - depth_ratio * 0.3f;
    const float camera_scaled_radius = radius * std::max(0.5f, std::min(2.0f, perspective_scale));

    // Also apply height-based darkening (lower Y = darker, as if in shadow)
    const int height_darkness = static_cast<int>(std::max(0.0f, std::min(40.0f, -height_y * 0.02f)));
    const int depth_darkness = static_cast<int>(std::max(0.0f, std::min(30.0f, depth_ratio * 30.0f)));
    const int total_darkness = height_darkness + depth_darkness;

    SDL_Color point_color = get_axis_color(axis);
    const float center_radius = camera_scaled_radius * 0.4f;

    // Only draw larger outer circle if selected
    if (is_selected) {
        SDL_Color line_color = point_color;
        line_color.a = 140;
        const float length = 180.0f * std::max(0.5f, std::min(2.0f, perspective_scale));
        render_axis_line(renderer, screen_pos, axis, length, line_color.a);

        const float outer_radius = camera_scaled_radius * 1.5f;

        // Apply depth and height shading
        SDL_Color outer_color = point_color;
        outer_color.r = static_cast<Uint8>(std::max(0, outer_color.r - total_darkness));
        outer_color.g = static_cast<Uint8>(std::max(0, outer_color.g - total_darkness));
        outer_color.b = static_cast<Uint8>(std::max(0, outer_color.b - total_darkness));
        SDL_SetRenderDrawColor(renderer, outer_color.r, outer_color.g, outer_color.b, 255);

        // Draw larger outer circle with camera perspective
        for (float y = -outer_radius; y <= outer_radius; ++y) {
            for (float x = -outer_radius; x <= outer_radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= outer_radius * outer_radius) {
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
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
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }
    }

    // Always draw small center point (always visible, fixed size)
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    const float fixed_center_radius = radius * 0.4f;
    for (float y = -fixed_center_radius; y <= fixed_center_radius; ++y) {
        for (float x = -fixed_center_radius; x <= fixed_center_radius; ++x) {
            if (x * x + y * y <= fixed_center_radius * fixed_center_radius) {
                SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
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
            constrained.x = 0.0f;  // Y is height (up/down) so drag vertically
            break;
        case AdjustmentAxis::Z:
            constrained.x = 0.0f;  // Depth adjustments driven by drag along screen Y
            break;
    }
    return constrained;
}

void Point3DEditor::render_axis_line(SDL_Renderer* renderer,
                                     SDL_FPoint screen_pos,
                                     AdjustmentAxis axis,
                                     float length,
                                     Uint8 alpha) {
    if (!renderer) return;
    SDL_FPoint dir;
    switch (axis) {
        case AdjustmentAxis::X: dir = SDL_FPoint{1.0f, 0.0f}; break;
        case AdjustmentAxis::Y: dir = SDL_FPoint{0.0f, 1.0f}; break;
        case AdjustmentAxis::Z: dir = SDL_FPoint{0.0f, -1.0f}; break;
    }
    SDL_Color axis_color = get_axis_color(axis);
    SDL_Color line_color = axis_color;
    line_color.a = alpha;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, line_color.r, line_color.g, line_color.b, line_color.a);
    SDL_FPoint a{screen_pos.x - dir.x * length, screen_pos.y - dir.y * length};
    SDL_FPoint b{screen_pos.x + dir.x * length, screen_pos.y + dir.y * length};
    SDL_RenderLine(renderer, a.x, a.y, b.x, b.y);
}

SDL_Color Point3DEditor::get_axis_color(AdjustmentAxis axis) {
    switch (axis) {
        case AdjustmentAxis::X:
            return SDL_Color{255, 50, 50, 255};  // Red (left/right)
        case AdjustmentAxis::Y:
            return SDL_Color{50, 255, 50, 255};  // Green (height - up/down)
        case AdjustmentAxis::Z:
            return SDL_Color{50, 150, 255, 255};  // Blue (depth - forward/back)
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
                SDL_RenderLine(renderer, x1, y1 + offset, x2, y2 + offset);
            } else {
                // More vertical - offset horizontally
                SDL_RenderLine(renderer, x1 + offset, y1, x2 + offset, y2);
            }
        }
    };

    switch (axis) {
        case AdjustmentAxis::X: {
            // Horizontal arrows (left/right)
            // Main line
            draw_thick_line(center.x - half_length, center.y, center.x + half_length, center.y);

            // Left arrow
            SDL_RenderLine(renderer, center.x - half_length, center.y,
                             center.x - half_length + arrow_size, center.y - arrow_size);
            SDL_RenderLine(renderer, center.x - half_length, center.y,
                             center.x - half_length + arrow_size, center.y + arrow_size);

            // Right arrow
            SDL_RenderLine(renderer, center.x + half_length, center.y,
                             center.x + half_length - arrow_size, center.y - arrow_size);
            SDL_RenderLine(renderer, center.x + half_length, center.y,
                             center.x + half_length - arrow_size, center.y + arrow_size);
            break;
        }
        case AdjustmentAxis::Y:
        case AdjustmentAxis::Z: {
            // Vertical arrows (up/down) - Z uses same as Y since it moves vertically on screen
            // Main line
            draw_thick_line(center.x, center.y - half_length, center.x, center.y + half_length);

            // Up arrow
            SDL_RenderLine(renderer, center.x, center.y - half_length,
                             center.x - arrow_size, center.y - half_length + arrow_size);
            SDL_RenderLine(renderer, center.x, center.y - half_length,
                             center.x + arrow_size, center.y - half_length + arrow_size);

            // Down arrow
            SDL_RenderLine(renderer, center.x, center.y + half_length,
                             center.x - arrow_size, center.y + half_length - arrow_size);
            SDL_RenderLine(renderer, center.x, center.y + half_length,
                             center.x + arrow_size, center.y + half_length - arrow_size);
            break;
        }
    }
}

bool Point3DEditor::handle_mouse_event(const SDL_Event& e,
                                      const std::vector<SDL_FPoint>& point_screens,
                                      const std::vector<bool>& point_selectable) {
    // Allow initial click-selection when no target is active yet.
    // MovementFrameEditor starts with SelectionTarget::None and promotes
    // to MovementPoint via on_point_selected_ once a point is clicked.
    if (!selection_) {
        return false;
    }

    auto axis_dir = [this]() -> SDL_FPoint {
        switch (selection_->axis) {
            case AdjustmentAxis::X: return SDL_FPoint{1.0f, 0.0f};
            case AdjustmentAxis::Y: return SDL_FPoint{0.0f, 1.0f};
            case AdjustmentAxis::Z: return SDL_FPoint{0.0f, -1.0f};
        }
        return SDL_FPoint{1.0f, 0.0f};
    };

    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point mouse_pos = sdl_mouse_util::MotionPoint(e.motion);
        int new_hover = -1;

        for (std::size_t i = 0; i < point_screens.size(); ++i) {
            const bool is_selectable = (i < point_selectable.size() && point_selectable[i]);
            if (is_selectable && handle_point_click(mouse_pos, point_screens[i])) {
                new_hover = static_cast<int>(i);
                break;
            }
        }

        if (new_hover != hovered_point_index_) {
            hovered_point_index_ = new_hover;
        }

        if (is_dragging_ && selected_point_index_ >= 0 && on_position_changed_) {
            if (!is_axis_enabled(selection_->axis) ||
                axis_locked_values_[axis_to_index(selection_->axis)].has_value()) {
                return true;
            }
            SDL_FPoint dir = axis_dir();
            const float len = std::max(0.001f, std::sqrt(dir.x * dir.x + dir.y * dir.y));
            dir.x /= len;
            dir.y /= len;

            const float dx = static_cast<float>(mouse_pos.x - drag_start_screen_.x);
            const float dy = static_cast<float>(mouse_pos.y - drag_start_screen_.y);
            const float projected = dx * dir.x + dy * dir.y;

            float step = grid_step_world_ > 0.0f ? grid_step_world_ : 1.0f;
            float world_delta = std::round(projected / step) * step;

            SDL_FPoint new_world = drag_start_world_;
            float new_world_z = drag_start_world_z_;
            switch (selection_->axis) {
                case AdjustmentAxis::X:
                    new_world.x += world_delta;
                    break;
                case AdjustmentAxis::Y:
                    new_world.y += world_delta;
                    break;
                case AdjustmentAxis::Z:
                    new_world_z += world_delta;
                    break;
            }
            on_position_changed_(new_world, new_world_z);
            return true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point mouse_pos = sdl_mouse_util::ButtonPoint(e.button);
        Uint32 current_time = SDL_GetTicks();

        for (std::size_t i = 0; i < point_screens.size(); ++i) {
            if (handle_point_click(mouse_pos, point_screens[i])) {
                const bool is_selectable = (i < point_selectable.size() && point_selectable[i]);
                if (!is_selectable) {
                    return false;
                }
                const int clicked_index = static_cast<int>(i);

                if (clicked_index == last_clicked_point_ &&
                    clicked_index == selected_point_index_ &&
                    (current_time - last_click_time_) < DOUBLE_CLICK_THRESHOLD_MS) {
                    cycle_axis();
                    last_click_time_ = 0;
                    last_clicked_point_ = -1;
                } else {
                    last_click_time_ = current_time;
                    last_clicked_point_ = clicked_index;

                    if (clicked_index != selected_point_index_) {
                        selected_point_index_ = clicked_index;
                        if (on_point_selected_) {
                            on_point_selected_(clicked_index);
                        }
                    }
                }

                // Start drag on selected point
                if (selected_point_index_ >= 0 &&
                    is_axis_enabled(selection_->axis) &&
                    !axis_locked_values_[axis_to_index(selection_->axis)].has_value()) {
                    is_dragging_ = true;
                    drag_start_screen_ = mouse_pos;
                    drag_start_world_ = selection_->world_pos;
                    drag_start_world_z_ = selection_->world_z;
                }
                return true;
            }
        }

        selected_point_index_ = -1;
        hovered_point_index_ = -1;
        last_click_time_ = 0;
        last_clicked_point_ = -1;
        is_dragging_ = false;
        if (on_point_selected_) {
            on_point_selected_(-1);
        }
        return false;
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        is_dragging_ = false;
    }

    return is_dragging_;
}

void Point3DEditor::render_selectable_point(SDL_Renderer* renderer,
                                           SDL_FPoint screen_pos,
                                           bool is_selected,
                                           bool is_hovered,
                                           float radius) {
    if (!renderer) return;

    const SDL_Color orange{255, 165, 0, 255};
    const float center_radius = radius * 0.4f;
    const AdjustmentAxis active_axis = selection_ ? selection_->axis : AdjustmentAxis::X;

    // If selected, draw larger outer circle with axis color
    if (is_selected) {
        render_axis_line(renderer, screen_pos, active_axis, 180.0f);
        const float outer_radius = radius * 1.5f;
        SDL_Color axis_color = get_axis_color(active_axis);

        // Draw outer circle with axis color
        SDL_SetRenderDrawColor(renderer, axis_color.r, axis_color.g, axis_color.b, 255);
        for (float y = -outer_radius; y <= outer_radius; ++y) {
            for (float x = -outer_radius; x <= outer_radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= outer_radius * outer_radius) {
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }

        // White border
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        const float border_inner = outer_radius;
        const float border_outer = outer_radius + 1.5f;
        for (float y = -border_outer; y <= border_outer; ++y) {
            for (float x = -border_outer; x <= border_outer; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq >= border_inner * border_inner && dist_sq <= border_outer * border_outer) {
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }

        // Movement arrows
        render_movement_arrows(renderer, screen_pos, active_axis, 32.0f);
    }
    // If hovered (but not selected), draw white outline
    else if (is_hovered) {
        // Draw orange circle
        SDL_SetRenderDrawColor(renderer, orange.r, orange.g, orange.b, 255);
        for (float y = -radius; y <= radius; ++y) {
            for (float x = -radius; x <= radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= radius * radius) {
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }

        // White outline
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        const float outline_inner = radius;
        const float outline_outer = radius + 2.0f;
        for (float y = -outline_outer; y <= outline_outer; ++y) {
            for (float x = -outline_outer; x <= outline_outer; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq >= outline_inner * outline_inner && dist_sq <= outline_outer * outline_outer) {
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }
    }
    // Otherwise, just draw orange circle
    else {
        SDL_SetRenderDrawColor(renderer, orange.r, orange.g, orange.b, 255);
        for (float y = -radius; y <= radius; ++y) {
            for (float x = -radius; x <= radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= radius * radius) {
                    SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }
    }

    // Always draw white center dot
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    for (float y = -center_radius; y <= center_radius; ++y) {
        for (float x = -center_radius; x <= center_radius; ++x) {
            if (x * x + y * y <= center_radius * center_radius) {
                SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }
}

void Point3DEditor::render_non_selectable_point(SDL_Renderer* renderer,
                                                SDL_FPoint screen_pos,
                                                float radius) {
    if (!renderer) return;

    const SDL_Color gray{128, 128, 128, 128};

    // Semi-transparent gray circle
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, gray.r, gray.g, gray.b, gray.a);

    for (float y = -radius; y <= radius; ++y) {
        for (float x = -radius; x <= radius; ++x) {
            float dist_sq = x * x + y * y;
            if (dist_sq <= radius * radius) {
                SDL_RenderPoint(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

}

