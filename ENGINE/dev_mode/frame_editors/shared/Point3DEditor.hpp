#pragma once

#include <SDL.h>
#include <functional>
#include <memory>
#include <string>

#include "SelectionState.hpp"

class DMTextBox;

namespace devmode::frame_editors {

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

    // UI rendering and event handling
    bool handle_event(const SDL_Event& e, const SDL_Rect& container);
    void render_overlays(SDL_Renderer* renderer, const SDL_Rect& container);
    int get_overlay_height() const;

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
                           std::function<SDL_FPoint(const SDL_Point&)> screen_to_world);

    // Point rendering methods
    void render_axis_point(SDL_Renderer* renderer,
                          SDL_FPoint screen_pos,
                          AdjustmentAxis axis,
                          bool is_selected,
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
    bool is_dragging_ = false;
    int selected_point_index_ = -1;
    bool point_already_selected_ = false;  // Track if clicking already-selected point
    SDL_Point drag_start_mouse_pos_;
    SDL_FPoint drag_start_world_pos_;
    float drag_start_world_z_ = 0.0f;

    // Rendering helper methods
    SDL_Color get_axis_color(AdjustmentAxis axis);
    void render_movement_arrows(SDL_Renderer* renderer,
                               SDL_FPoint center,
                               AdjustmentAxis axis,
                               float length = 32.0f);
};

}
