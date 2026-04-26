#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

class DMButton;
class DMTextBox;
class DMCheckbox;
class DMNumericStepper;

class RoomBoxToolsPanel {
public:
    enum class Kind {
        HitBox,
        AttackBox,
        ImpassableBox,
    };

    enum class PropagationScope {
        NextFrame,
        Animation,
        Asset,
    };

    struct DetailValues {
        std::string name;
        int extrusion_forward = 1;
        int extrusion_backward = 1;
        int damage = 0;
    };

    using SelectCallback = std::function<void(int)>;
    using AddCallback = std::function<void()>;
    using DeleteCallback = std::function<void()>;
    using ApplyCallback = std::function<void(const DetailValues&)>;
    using PropagateCallback = std::function<void(PropagationScope)>;
    using SystemEnabledToggleCallback = std::function<void(bool)>;
    using IncrementPointCountCallback = std::function<void()>;
    using DecrementPointCountCallback = std::function<void()>;

    explicit RoomBoxToolsPanel(Kind kind);
    ~RoomBoxToolsPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();

    void set_box_names(const std::vector<std::string>& names);
    void set_selection(int box_index, int corner_index);
    void clear_selection();
    void set_name_text(const std::string& value);
    void set_detail_values(const DetailValues& values);
    void set_system_enabled(bool enabled);
    bool system_enabled() const;
    void set_propagation_visible(bool visible);
    void set_point_count(int count);

    void set_on_select(SelectCallback callback);
    void set_on_add(AddCallback callback);
    void set_on_delete(DeleteCallback callback);
    void set_on_apply(ApplyCallback callback);
    void set_on_propagate(PropagateCallback callback);
    void set_on_system_enabled_toggle(SystemEnabledToggleCallback callback);
    void set_on_increment_point_count(IncrementPointCountCallback callback);
    void set_on_decrement_point_count(DecrementPointCountCallback callback);

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    void update_layout() const;
    void layout_box_buttons() const;
    void scroll_by(int delta);
    DetailValues collect_detail_values() const;
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);

private:
    Kind kind_ = Kind::HitBox;
    bool propagation_visible_ = true;
    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;

    mutable bool layout_dirty_ = true;
    bool panel_bounds_override_active_ = false;
    SDL_Rect panel_bounds_override_{0, 0, 0, 0};
    mutable SDL_Rect panel_rect_{12, 56, 320, 520};
    mutable SDL_Rect header_rect_{0, 0, 0, 0};
    mutable SDL_Rect enabled_toggle_rect_{0, 0, 0, 0};
    mutable SDL_Rect subtitle_rect_{0, 0, 0, 0};
    mutable SDL_Rect list_clip_rect_{0, 0, 0, 0};
    mutable SDL_Rect detail_title_rect_{0, 0, 0, 0};
    mutable SDL_Rect corner_label_rect_{0, 0, 0, 0};
    mutable SDL_Rect point_count_rect_{0, 0, 0, 0};
    mutable int content_height_ = 0;
    mutable int max_scroll_ = 0;
    mutable int scroll_offset_ = 0;

    std::vector<std::string> box_names_;
    int selected_box_index_ = -1;
    int selected_corner_index_ = 0;
    std::vector<std::unique_ptr<DMButton>> box_buttons_;

    std::unique_ptr<DMButton> add_button_;
    std::unique_ptr<DMButton> delete_button_;
    std::unique_ptr<DMButton> apply_next_frame_button_;
    std::unique_ptr<DMButton> apply_animation_button_;
    std::unique_ptr<DMButton> apply_asset_button_;
    std::unique_ptr<DMTextBox> name_textbox_;
    std::unique_ptr<DMTextBox> extrusion_forward_textbox_;
    std::unique_ptr<DMTextBox> extrusion_backward_textbox_;
    std::unique_ptr<DMTextBox> damage_textbox_;
    std::unique_ptr<DMNumericStepper> point_count_stepper_;
    std::unique_ptr<DMCheckbox> system_enabled_checkbox_;
    int point_count_ = 0;

    SelectCallback on_select_;
    AddCallback on_add_;
    DeleteCallback on_delete_;
    ApplyCallback on_apply_;
    PropagateCallback on_propagate_;
    SystemEnabledToggleCallback on_system_enabled_toggle_;
    IncrementPointCountCallback on_increment_point_count_;
    DecrementPointCountCallback on_decrement_point_count_;

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
    friend struct RoomBoxToolsPanelTestAccess;
#endif
};

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
struct RoomBoxToolsPanelTestAccess {
    static bool delete_button_visible(RoomBoxToolsPanel& panel);
    static SDL_Rect delete_button_rect(RoomBoxToolsPanel& panel);
};
#endif
