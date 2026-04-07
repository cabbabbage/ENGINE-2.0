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

class RoomAnchorToolsPanel {
public:
    enum class PropagationScope {
        NextFrame,
        Animation,
        Asset,
    };

    struct DetailValues {
        float depth_offset = 0.0f;
        bool flip_horizontal = true;
        bool flip_vertical = true;
        float rotation_degrees = 0.0f;
        bool hidden = false;
        bool resolve_x = true;
        AnchorScalingMethod scaling_method = AnchorScalingMethod::Parent;
    };

    struct LightValues {
        bool has_light_data = false;
        bool enabled = false;
        int color_r = 255;
        int color_g = 236;
        int color_b = 196;
        float intensity = 1.0f;
        float radius = 220.0f;
        float falloff = 1.8f;
        float shadow_strength = 0.82f;
        bool cast_shadows = true;
    };

    using SelectCallback = std::function<void(const std::string&)>;
    using AddCallback = std::function<void()>;
    using RenameCallback = std::function<void(const std::string&)>;
    using DeleteCallback = std::function<void()>;
    using ApplyDetailsCallback = std::function<void(const DetailValues&)>;
    using ApplyLightDetailsCallback = std::function<void(const LightValues&)>;
    using PropagateCallback = std::function<void(PropagationScope)>;
    using OnionSkinToggleCallback = std::function<void(bool)>;
    using OpenCandidatesCallback = std::function<void(const std::string&, SDL_Point, SDL_Rect)>;

    RoomAnchorToolsPanel();
    ~RoomAnchorToolsPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();
    void set_anchor_names(const std::vector<std::string>& names);
    void set_selected_anchor(const std::string& name);
    const std::string& selected_anchor() const { return selected_anchor_name_; }
    void set_rename_text(const std::string& value);
    std::string rename_text() const;
    void set_detail_values(const DetailValues& values);
    void set_light_editor_mode(bool enabled);
    bool light_editor_mode() const { return light_editor_mode_; }
    void set_light_values(const LightValues& values);
    void set_onion_skin_enabled(bool enabled);
    bool onion_skin_enabled() const;

    void set_on_select(SelectCallback callback);
    void set_on_add(AddCallback callback);
    void set_on_rename(RenameCallback callback);
    void set_on_delete(DeleteCallback callback);
    void set_on_apply_details(ApplyDetailsCallback callback);
    void set_on_apply_light_details(ApplyLightDetailsCallback callback);
    void set_on_propagate(PropagateCallback callback);
    void set_on_onion_skin_toggle(OnionSkinToggleCallback callback);
    void set_on_open_candidates(OpenCandidatesCallback callback);

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    void update_layout() const;
    void layout_anchor_buttons() const;
    void scroll_by(int delta);
    DetailValues collect_detail_values() const;
    LightValues collect_light_values() const;
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);

private:
    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;

    mutable bool layout_dirty_ = true;
    bool panel_bounds_override_active_ = false;
    SDL_Rect panel_bounds_override_{0, 0, 0, 0};
    mutable SDL_Rect panel_rect_{12, 56, 300, 420};
    mutable SDL_Rect light_panel_rect_{0, 0, 0, 0};
    mutable SDL_Rect header_rect_{0, 0, 0, 0};
    mutable SDL_Rect light_header_rect_{0, 0, 0, 0};
    mutable SDL_Rect detail_title_rect_{0, 0, 0, 0};
    mutable SDL_Rect advanced_card_rect_{0, 0, 0, 0};
    mutable SDL_Rect light_title_rect_{0, 0, 0, 0};
    mutable SDL_Rect light_card_rect_{0, 0, 0, 0};
    mutable SDL_Rect list_clip_rect_{0, 0, 0, 0};
    mutable int content_height_ = 0;
    mutable int max_scroll_ = 0;
    mutable int scroll_offset_ = 0;

    std::vector<std::string> anchor_names_;
    std::string selected_anchor_name_;
    std::vector<std::unique_ptr<DMButton>> anchor_buttons_;

    std::unique_ptr<DMButton> add_button_;
    std::unique_ptr<DMTextBox> rename_textbox_;
    std::unique_ptr<DMTextBox> depth_textbox_;
    std::unique_ptr<DMSlider> rotation_slider_;
    std::unique_ptr<DMCheckbox> hidden_checkbox_;
    std::unique_ptr<DMButton> advanced_options_button_;
    std::unique_ptr<DMCheckbox> flip_horizontal_checkbox_;
    std::unique_ptr<DMCheckbox> flip_vertical_checkbox_;
    std::unique_ptr<DMCheckbox> resolve_x_checkbox_;
    std::unique_ptr<DMDropdown> scaling_method_dropdown_;
    std::unique_ptr<DMCheckbox> light_enabled_checkbox_;
    std::unique_ptr<DMTextBox> light_color_r_textbox_;
    std::unique_ptr<DMTextBox> light_color_g_textbox_;
    std::unique_ptr<DMTextBox> light_color_b_textbox_;
    std::unique_ptr<DMSlider> light_intensity_slider_;
    std::unique_ptr<DMSlider> light_radius_slider_;
    std::unique_ptr<DMSlider> light_falloff_slider_;
    std::unique_ptr<DMSlider> light_shadow_strength_slider_;
    std::unique_ptr<DMCheckbox> light_cast_shadows_checkbox_;
    std::unique_ptr<DMCheckbox> onion_skin_checkbox_;
    std::unique_ptr<DMButton> delete_button_;
    std::unique_ptr<DMButton> apply_next_frame_button_;
    std::unique_ptr<DMButton> apply_animation_button_;
    std::unique_ptr<DMButton> apply_asset_button_;
    bool advanced_options_expanded_ = false;
    bool light_editor_mode_ = false;

    SelectCallback on_select_;
    AddCallback on_add_;
    RenameCallback on_rename_;
    DeleteCallback on_delete_;
    ApplyDetailsCallback on_apply_details_;
    ApplyLightDetailsCallback on_apply_light_details_;
    PropagateCallback on_propagate_;
    OnionSkinToggleCallback on_onion_skin_toggle_;
    OpenCandidatesCallback on_open_candidates_;
};
