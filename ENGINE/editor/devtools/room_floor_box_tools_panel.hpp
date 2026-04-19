#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

class DMButton;
class DMCheckbox;
class DMTextBox;

class RoomFloorBoxToolsPanel {
public:
    struct DetailValues {
        std::string name;
        bool enabled = true;
        float position_x = 0.0f;
        float position_z = 0.0f;
        float width = 0.0f;
        float depth = 0.0f;
        std::vector<std::string> tags;
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
    void set_recommendation_pool(const std::vector<std::string>& tags);

    void set_on_select(SelectCallback callback);
    void set_on_add(AddCallback callback);
    void set_on_delete(DeleteCallback callback);
    void set_on_apply(ApplyCallback callback);
    void set_on_system_enabled_toggle(SystemEnabledToggleCallback callback);

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    struct TagChip {
        std::string value;
        std::unique_ptr<DMButton> button;
    };

    void update_layout() const;
    void layout_box_buttons() const;
    int layout_tag_chips(std::vector<TagChip>& chips,
                         int content_x,
                         int content_w,
                         int start_y,
                         bool apply) const;
    void scroll_by(int delta);
    DetailValues collect_detail_values() const;
    static std::string normalize_tag(std::string_view raw);
    static std::string to_lower_copy(std::string value);
    static bool starts_with(std::string_view value, std::string_view prefix);
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);
    static float parse_float_or(const std::string& text, float fallback);
    static std::string format_float(float value);
    void set_tag_values(const std::vector<std::string>& tags);
    void refresh_recommendations();
    void rebuild_tag_buttons();
    void add_tag(const std::string& tag);
    void remove_tag(const std::string& tag);

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
    mutable SDL_Rect tags_label_rect_{0, 0, 0, 0};
    mutable SDL_Rect recommended_label_rect_{0, 0, 0, 0};
    mutable int content_height_ = 0;
    mutable int max_scroll_ = 0;
    mutable int scroll_offset_ = 0;

    std::vector<std::string> box_names_;
    int selected_box_index_ = -1;
    std::vector<std::unique_ptr<DMButton>> box_buttons_;
    std::set<std::string> active_tags_;
    std::vector<std::string> recommendation_pool_;
    std::vector<std::string> recommended_tags_;
    std::string search_input_;
    std::string search_query_;
    std::vector<TagChip> active_tag_chips_;
    std::vector<TagChip> recommended_tag_chips_;

    std::unique_ptr<DMButton> add_button_;
    std::unique_ptr<DMButton> delete_button_;
    std::unique_ptr<DMTextBox> name_textbox_;
    std::unique_ptr<DMCheckbox> enabled_checkbox_;
    std::unique_ptr<DMTextBox> position_x_textbox_;
    std::unique_ptr<DMTextBox> position_z_textbox_;
    std::unique_ptr<DMTextBox> width_textbox_;
    std::unique_ptr<DMTextBox> depth_textbox_;
    std::unique_ptr<DMTextBox> tag_search_textbox_;
    std::unique_ptr<DMCheckbox> system_enabled_checkbox_;

    SelectCallback on_select_;
    AddCallback on_add_;
    DeleteCallback on_delete_;
    ApplyCallback on_apply_;
    SystemEnabledToggleCallback on_system_enabled_toggle_;
};
