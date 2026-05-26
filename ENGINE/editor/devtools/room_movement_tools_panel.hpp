#pragma once

#include <memory>
#include <string>
#include <functional>
#include <vector>

#include <SDL3/SDL.h>

class DMCheckbox;
class DMTextBox;
class DMButton;

class RoomMovementToolsPanel {
public:
    using SystemEnabledToggleCallback = std::function<void(bool)>;
    using PathSelectionChangedCallback = std::function<void(int)>;
    using PathActionCallback = std::function<void()>;
    struct QuantizeOption {
        std::string label;
        bool current_animation = false;
        bool selectable = true;
    };
    struct NumericValues {
        float dx = 0.0f;
        float dy = 0.0f;
        float dz = 0.0f;
        float rotation_degrees = 0.0f;
    };

    RoomMovementToolsPanel();
    ~RoomMovementToolsPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();

    void set_smooth_enabled(bool enabled);
    bool smooth_enabled() const;
    void set_curve_enabled(bool enabled);
    bool curve_enabled() const;
    void set_system_enabled(bool enabled);
    bool system_enabled() const;
    void set_numeric_values(const NumericValues& values);
    NumericValues numeric_values() const;
    bool any_numeric_editing() const;
    void set_on_system_enabled_toggle(SystemEnabledToggleCallback callback);
    void set_path_options(const std::vector<std::string>& options, int selected_index);
    void set_on_path_selection_changed(PathSelectionChangedCallback callback);
    void set_on_add_path(PathActionCallback callback);
    void set_on_delete_path(PathActionCallback callback);
    void set_quantize_options(const std::vector<QuantizeOption>& options);
    void set_on_quantize_path_selected(PathSelectionChangedCallback callback);

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    void update_layout() const;
    void layout_path_buttons() const;
    void scroll_paths_by(int delta);
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);

private:
    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;
    bool panel_bounds_override_active_ = false;
    SDL_Rect panel_bounds_override_{0, 0, 0, 0};

    mutable bool layout_dirty_ = true;
    mutable SDL_Rect panel_rect_{12, 56, 300, 210};
    mutable SDL_Rect header_rect_{0, 0, 0, 0};
    mutable SDL_Rect enabled_rect_{0, 0, 0, 0};
    mutable SDL_Rect hint_rect_{0, 0, 0, 0};
    mutable SDL_Rect smooth_rect_{0, 0, 0, 0};
    mutable SDL_Rect curve_rect_{0, 0, 0, 0};
    mutable SDL_Rect dx_rect_{0, 0, 0, 0};
    mutable SDL_Rect dy_rect_{0, 0, 0, 0};
    mutable SDL_Rect dz_rect_{0, 0, 0, 0};
    mutable SDL_Rect rot_rect_{0, 0, 0, 0};
    mutable SDL_Rect path_select_rect_{0, 0, 0, 0};
    mutable SDL_Rect path_list_rect_{0, 0, 0, 0};
    mutable SDL_Rect path_add_rect_{0, 0, 0, 0};
    mutable SDL_Rect path_delete_rect_{0, 0, 0, 0};
    mutable SDL_Rect quantize_rect_{0, 0, 0, 0};
    mutable SDL_Rect quantize_panel_rect_{0, 0, 0, 0};
    mutable int path_content_height_ = 0;
    mutable int path_max_scroll_ = 0;
    mutable int path_scroll_offset_ = 0;

    std::unique_ptr<DMCheckbox> enabled_checkbox_;
    std::unique_ptr<DMCheckbox> smooth_checkbox_;
    std::unique_ptr<DMCheckbox> curve_checkbox_;
    std::unique_ptr<DMTextBox> dx_box_;
    std::unique_ptr<DMTextBox> dy_box_;
    std::unique_ptr<DMTextBox> dz_box_;
    std::unique_ptr<DMTextBox> rot_box_;
    std::vector<std::string> path_options_;
    std::vector<std::unique_ptr<DMButton>> path_buttons_;
    int selected_path_index_ = 0;
    std::unique_ptr<DMButton> add_path_button_;
    std::unique_ptr<DMButton> delete_path_button_;
    std::unique_ptr<DMButton> quantize_button_;
    std::vector<QuantizeOption> quantize_options_;
    mutable std::vector<SDL_Rect> quantize_option_rects_;
    int quantize_hover_index_ = -1;
    bool quantize_open_ = false;
    PathSelectionChangedCallback on_quantize_path_selected_;
    SystemEnabledToggleCallback on_system_enabled_toggle_;
    PathSelectionChangedCallback on_path_selection_changed_;
    PathActionCallback on_add_path_;
    PathActionCallback on_delete_path_;
};
