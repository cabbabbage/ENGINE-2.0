#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "assets/asset/anchor_point.hpp"

class DMButton;
class DMTextBox;
class DMSlider;
class DMCheckbox;
class DMDropdown;

class RoomOvalToolsPanel {
public:
    struct OvalProperties {
        std::string name;
        float width_radius_x = 48.0f;
        float height_radius_z = 24.0f;
        std::string asset_name;
    };

    struct PointDetailValues {
        float rotation_degrees = 0.0f;
        bool hidden = false;
        bool resolve_x = true;
        bool flip_horizontal = true;
        bool flip_vertical = true;
        AnchorScalingMethod scaling_method = AnchorScalingMethod::Parent;
    };

    enum class AssetBindingStatusKind {
        None = 0,
        ExplicitCandidates = 1,
        OvalFallback = 2,
        Missing = 3,
        Invalid = 4,
    };

    struct AssetBindingStatus {
        AssetBindingStatusKind kind = AssetBindingStatusKind::None;
        std::string detail;
    };

    using SelectOvalCallback = std::function<void(int)>;
    using AddOvalCallback = std::function<void()>;
    using DeleteOvalCallback = std::function<void()>;
    using ApplyOvalPropertiesCallback = std::function<void(const OvalProperties&)>;
    using JumpToCenterAnchorCallback = std::function<void()>;
    using OpenCandidatesCallback = std::function<void(const std::string&, SDL_Point, SDL_Rect)>;

    using SelectPointCallback = std::function<void(int)>;
    using AddPointCallback = std::function<void()>;
    using DeletePointCallback = std::function<void()>;
    using ApplyPointDetailsCallback = std::function<void(const PointDetailValues&)>;

    RoomOvalToolsPanel();
    ~RoomOvalToolsPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();

    void set_oval_names(const std::vector<std::string>& names);
    void set_selected_oval_index(int index);
    int selected_oval_index() const { return selected_oval_index_; }
    void set_oval_properties(const OvalProperties& properties);
    void set_asset_binding_status(const AssetBindingStatus& status);
    void set_center_anchor_status(const std::string& center_name, bool present);

    void set_point_names(const std::vector<std::string>& names);
    void set_selected_point_index(int index);
    int selected_point_index() const { return selected_point_index_; }
    void set_point_detail_values(const PointDetailValues& values);

    void set_on_select_oval(SelectOvalCallback callback);
    void set_on_add_oval(AddOvalCallback callback);
    void set_on_delete_oval(DeleteOvalCallback callback);
    void set_on_apply_oval_properties(ApplyOvalPropertiesCallback callback);
    void set_on_jump_to_center_anchor(JumpToCenterAnchorCallback callback);
    void set_on_open_candidates(OpenCandidatesCallback callback);

    void set_on_select_point(SelectPointCallback callback);
    void set_on_add_point(AddPointCallback callback);
    void set_on_delete_point(DeletePointCallback callback);
    void set_on_apply_point_details(ApplyPointDetailsCallback callback);

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    void update_layout() const;
    void layout_oval_buttons() const;
    void layout_point_buttons() const;
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);
    static float parse_float_or(const std::string& text, float fallback);
    static std::string format_float(float value);
    static int scaling_method_to_dropdown_index(AnchorScalingMethod method);
    static AnchorScalingMethod dropdown_index_to_scaling_method(int index);
    PointDetailValues collect_point_detail_values() const;
    OvalProperties collect_oval_properties() const;

private:
    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;
    mutable bool layout_dirty_ = true;

    bool panel_bounds_override_active_ = false;
    SDL_Rect panel_bounds_override_{0, 0, 0, 0};
    mutable SDL_Rect panel_rect_{12, 56, 340, 760};

    mutable SDL_Rect header_rect_{0, 0, 0, 0};
    mutable SDL_Rect oval_list_clip_rect_{0, 0, 0, 0};
    mutable SDL_Rect point_list_clip_rect_{0, 0, 0, 0};
    mutable SDL_Rect asset_status_rect_{0, 0, 0, 0};
    mutable SDL_Rect candidate_hint_rect_{0, 0, 0, 0};
    mutable SDL_Rect center_status_rect_{0, 0, 0, 0};
    mutable SDL_Rect point_detail_title_rect_{0, 0, 0, 0};
    mutable SDL_Rect advanced_card_rect_{0, 0, 0, 0};

    std::vector<std::string> oval_names_;
    std::vector<std::unique_ptr<DMButton>> oval_buttons_;
    int selected_oval_index_ = -1;
    OvalProperties oval_properties_{};
    AssetBindingStatus asset_binding_status_{};
    std::string center_anchor_name_;
    bool center_anchor_present_ = false;

    std::vector<std::string> point_names_;
    std::vector<std::unique_ptr<DMButton>> point_buttons_;
    int selected_point_index_ = -1;

    std::unique_ptr<DMButton> add_oval_button_;
    std::unique_ptr<DMTextBox> oval_name_textbox_;
    std::unique_ptr<DMTextBox> width_textbox_;
    std::unique_ptr<DMTextBox> height_textbox_;
    std::unique_ptr<DMTextBox> asset_name_textbox_;
    std::unique_ptr<DMButton> apply_oval_properties_button_;
    std::unique_ptr<DMButton> center_anchor_jump_button_;
    std::unique_ptr<DMButton> delete_oval_button_;

    std::unique_ptr<DMButton> add_point_button_;
    std::unique_ptr<DMButton> delete_point_button_;
    std::unique_ptr<DMSlider> point_rotation_slider_;
    std::unique_ptr<DMCheckbox> point_hidden_checkbox_;
    std::unique_ptr<DMButton> advanced_options_button_;
    std::unique_ptr<DMCheckbox> point_flip_horizontal_checkbox_;
    std::unique_ptr<DMCheckbox> point_flip_vertical_checkbox_;
    std::unique_ptr<DMCheckbox> point_resolve_x_checkbox_;
    std::unique_ptr<DMDropdown> point_scaling_method_dropdown_;
    bool advanced_options_expanded_ = false;

    SelectOvalCallback on_select_oval_;
    AddOvalCallback on_add_oval_;
    DeleteOvalCallback on_delete_oval_;
    ApplyOvalPropertiesCallback on_apply_oval_properties_;
    JumpToCenterAnchorCallback on_jump_to_center_anchor_;
    OpenCandidatesCallback on_open_candidates_;

    SelectPointCallback on_select_point_;
    AddPointCallback on_add_point_;
    DeletePointCallback on_delete_point_;
    ApplyPointDetailsCallback on_apply_point_details_;
};
