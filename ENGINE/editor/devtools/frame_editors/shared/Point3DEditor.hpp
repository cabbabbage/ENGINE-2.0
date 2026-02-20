#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "SelectionState.hpp"

class DMTextBox;

namespace devmode::frame_editors {

// How coordinate values are displayed and interpreted in textboxes
enum class CoordinateDisplayMode {
    RawDelta,    // Display raw value as integer (for movement mode)
    Percentage   // Display as percentage 0.0-1.0 of parent height (for children, hit/attack geo)
};

// Backwards compatibility alias
using ZDisplayMode = CoordinateDisplayMode;

class Point3DEditor {
public:
    Point3DEditor() = default;
    explicit Point3DEditor(SelectionState* selection);
    ~Point3DEditor();

    void set_selection(SelectionState* selection);

    void cycle_axis();

    AdjustmentAxis axis() const {
        return selection_ ? selection_->axis : AdjustmentAxis::X;
    }

    void reset_axis(AdjustmentAxis axis);

    void set_axis_from_textbox_click(int textbox_index);

    void set_grid_resolution(int resolution);

    // Set the parent asset's height in pixels (used when z_display_mode_ == Percentage)
    void set_parent_height(float height_px);
    float get_parent_height() const { return parent_height_px_; }

    // Set how X/Y values are displayed and interpreted
    // RawDelta: X/Y are raw delta values (integers) - for movement mode (default)
    // Percentage: X/Y are percentages (floats 0.0-1.0) of parent height - for children, hit/attack geo
    void set_xy_display_mode(CoordinateDisplayMode mode) { xy_display_mode_ = mode; }
    CoordinateDisplayMode get_xy_display_mode() const { return xy_display_mode_; }

    // Set how Z values are displayed and interpreted
    // RawDelta: Z is a raw delta value (like dx/dy) - for movement mode
    // Percentage: Z is a percentage (0.0-1.0) of parent height - for children, hit/attack geo
    void set_z_display_mode(CoordinateDisplayMode mode) { z_display_mode_ = mode; }
    CoordinateDisplayMode get_z_display_mode() const { return z_display_mode_; }

    // Axis enable/lock controls (e.g., to freeze depth in sync-child mode)
    void set_axis_enabled(AdjustmentAxis axis, bool enabled);
    void set_axis_locked_value(AdjustmentAxis axis, std::optional<float> locked_value);
    bool is_axis_enabled(AdjustmentAxis axis) const;

    // UI rendering and event handling
    bool handle_event(const SDL_Event& e, const SDL_Rect& container);
    void render_overlays(SDL_Renderer* renderer, const SDL_Rect& container);
    int get_overlay_height(int container_width) const;

    // Coordinate synchronization
    void sync_textboxes_from_selection();
    void apply_textbox_changes();

    // Callback for when coordinates are changed via textboxes
    void set_on_coordinates_changed(std::function<void()> callback) {
        on_coordinates_changed_ = callback;
    }

    // Position change callback for when points are moved via mouse
    void set_on_position_changed(std::function<void(const SDL_FPoint& new_world_pos, float new_world_z)> callback) {
        on_position_changed_ = callback;
    }

    // Callback for when a point is selected via click
    void set_on_point_selected(std::function<void(int index)> callback) {
        on_point_selected_ = callback;
    }

    // Mouse event handling for 3D point manipulation
    bool handle_mouse_event(const SDL_Event& e,
                           const std::vector<SDL_FPoint>& point_screens,
                           const std::vector<bool>& point_selectable);

    // Point rendering methods
    void render_axis_point(SDL_Renderer* renderer,
                          SDL_FPoint screen_pos,
                          AdjustmentAxis axis,
                          bool is_selected,
                          float radius = 8.0f);

    // Render a selectable point (orange)
    void render_selectable_point(SDL_Renderer* renderer,
                                 SDL_FPoint screen_pos,
                                 bool is_selected,
                                 bool is_hovered,
                                 float radius = 8.0f);

    // Render a non-selectable point (gray, transparent)
    void render_non_selectable_point(SDL_Renderer* renderer,
                                     SDL_FPoint screen_pos,
                                     float radius = 8.0f);

    // Enhanced rendering with depth-based warping for 3D realism
    void render_axis_point_with_depth(SDL_Renderer* renderer,
                                     SDL_FPoint screen_pos,
                                     float world_z,
                                     AdjustmentAxis axis,
                                     bool is_selected,
                                     float radius = 8.0f);

    // Camera-aware rendering with proper realism warping
    // Pass world position and camera to apply correct perspective scaling
    void render_axis_point_with_camera(SDL_Renderer* renderer,
                                      SDL_FPoint world_pos,
                                      float world_z,
                                      const void* camera_grid,  // WarpedScreenGrid*
                                      AdjustmentAxis axis,
                                      bool is_selected,
                                      float radius = 8.0f);

    bool handle_point_click(const SDL_Point& mouse_pos,
                           SDL_FPoint point_screen_pos,
                           float radius = 8.0f);

    SDL_FPoint constrain_drag_delta(const SDL_FPoint& drag_delta,
                                   AdjustmentAxis axis);

    bool is_dragging() const { return is_dragging_; }

    int get_selected_point_index() const { return selected_point_index_; }

    // Externally set the selected point index (for arrow key navigation)
    void set_selected_point_index(int index) {
        selected_point_index_ = index;
        last_click_time_ = 0;
        last_clicked_point_ = -1;
        is_dragging_ = false;
    }

    int get_hovered_point_index() const { return hovered_point_index_; }

    // Get the cached container rect from last render (for overlay hit testing)
    const SDL_Rect& get_cached_container() const { return cached_container_; }

private:
    SelectionState* selection_ = nullptr;

    // UI widgets
    std::unique_ptr<DMTextBox> tb_dx_;
    std::unique_ptr<DMTextBox> tb_dy_;
    std::unique_ptr<DMTextBox> tb_dz_;

    // Cache for text values to avoid unnecessary updates
    std::string last_dx_text_;
    std::string last_dy_text_;
    std::string last_dz_text_;

    // Callback for coordinate changes
    std::function<void()> on_coordinates_changed_;

    // Position change callback
    std::function<void(const SDL_FPoint& new_world_pos, float new_world_z)> on_position_changed_;

    // Point selection callback
    std::function<void(int index)> on_point_selected_;

    // Mouse handling state
    int selected_point_index_ = -1;
    int hovered_point_index_ = -1;
    bool is_dragging_ = false;  // Mouse drag state for potential dragging interactions.
    SDL_Point drag_start_screen_{0, 0};
    SDL_FPoint drag_start_world_{0.0f, 0.0f};
    float drag_start_world_z_ = 0.0f;

    int grid_resolution_ = 0;
    float grid_step_world_ = 1.0f;
    float parent_height_px_ = 0.0f;  // Parent asset height for percent calculations
    CoordinateDisplayMode xy_display_mode_ = CoordinateDisplayMode::RawDelta;  // How X/Y are displayed/interpreted
    CoordinateDisplayMode z_display_mode_ = CoordinateDisplayMode::RawDelta;  // How Z is displayed/interpreted

    // Axis configuration (enabled/locked values)
    std::array<bool, 3> axis_enabled_{{true, true, true}};
    std::array<std::optional<float>, 3> axis_locked_values_{};

    // Double-click detection for axis cycling
    Uint32 last_click_time_ = 0;
    int last_clicked_point_ = -1;
    static constexpr Uint32 DOUBLE_CLICK_THRESHOLD_MS = 300;

    // Cached container rect from last render for event handling
    SDL_Rect cached_container_{0, 0, 0, 0};

    // Rendering helper methods
    void render_axis_line(SDL_Renderer* renderer,
                          SDL_FPoint screen_pos,
                          AdjustmentAxis axis,
                          float length,
                          Uint8 alpha = 140);
    SDL_Color get_axis_color(AdjustmentAxis axis);
    void render_movement_arrows(SDL_Renderer* renderer,
                               SDL_FPoint center,
                               AdjustmentAxis axis,
                               float length = 32.0f);

    AdjustmentAxis first_enabled_axis() const;
    AdjustmentAxis next_enabled_axis(AdjustmentAxis current) const;
    int axis_to_index(AdjustmentAxis axis) const;
};

}

