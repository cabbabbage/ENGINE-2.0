#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

class DMButton;
class DMCheckbox;
class DMTextBox;

class RoomFloorBoxToolsPanel {
public:
    struct DetailValues {
        std::string name;
        bool is_boundary = false;
        bool enabled = true;
        float position_x = 0.0f;
        float position_z = 0.0f;
        float width = 0.0f;
        float depth = 0.0f;
        float rotation_degrees = 0.0f;
    };

    using SelectCallback = std::function<void(int)>;
    using AddCallback = std::function<void()>;
    using DeleteCallback = std::function<void()>;
    using ApplyCallback = std::function<void(const DetailValues&)>;
    using SystemEnabledToggleCallback = std::function<void(bool)>;

    RoomFloorBoxToolsPanel();
    ~RoomFloorBoxToolsPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();

    void set_system_enabled(bool enabled);
    bool system_enabled() const;
    void set_floor_box_names(const std::vector<std::string>& names);
    void set_selection(int box_index);
    void clear_selection();
    void set_detail_values(const DetailValues& values);

    void set_on_select(SelectCallback callback);
    void set_on_add(AddCallback callback);
    void set_on_delete(DeleteCallback callback);
    void set_on_apply(ApplyCallback callback);
    void set_on_system_enabled_toggle(SystemEnabledToggleCallback callback);

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    void update_layout() const;
    void layout_box_buttons() const;
    void scroll_by(int delta);
    DetailValues collect_detail_values() const;
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);
    static float parse_float_or(const std::string& text, float fallback);
    static std::string format_float(float value);

    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;

    mutable bool layout_dirty_ = true;
    bool panel_bounds_override_active_ = false;
    SDL_Rect panel_bounds_override_{0, 0, 0, 0};
    mutable SDL_Rect panel_rect_{12, 56, 320, 600};
    mutable SDL_Rect header_rect_{0, 0, 0, 0};
    mutable SDL_Rect enabled_toggle_rect_{0, 0, 0, 0};
    mutable SDL_Rect subtitle_rect_{0, 0, 0, 0};
    mutable SDL_Rect list_clip_rect_{0, 0, 0, 0};
    mutable SDL_Rect detail_title_rect_{0, 0, 0, 0};
    mutable int content_height_ = 0;
    mutable int max_scroll_ = 0;
    mutable int scroll_offset_ = 0;

    std::vector<std::string> box_names_;
    int selected_box_index_ = -1;
    std::vector<std::unique_ptr<DMButton>> box_buttons_;

    std::unique_ptr<DMButton> add_button_;
    std::unique_ptr<DMButton> delete_button_;
    std::unique_ptr<DMTextBox> name_textbox_;
    std::unique_ptr<DMCheckbox> boundary_checkbox_;
    std::unique_ptr<DMCheckbox> enabled_checkbox_;
    std::unique_ptr<DMTextBox> position_x_textbox_;
    std::unique_ptr<DMTextBox> position_z_textbox_;
    std::unique_ptr<DMTextBox> width_textbox_;
    std::unique_ptr<DMTextBox> depth_textbox_;
    std::unique_ptr<DMTextBox> rotation_textbox_;
    std::unique_ptr<DMCheckbox> system_enabled_checkbox_;

    SelectCallback on_select_;
    AddCallback on_add_;
    DeleteCallback on_delete_;
    ApplyCallback on_apply_;
    SystemEnabledToggleCallback on_system_enabled_toggle_;
};

