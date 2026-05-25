#pragma once

#include <functional>
#include <memory>

#include <SDL3/SDL.h>

#include "animation/controllers/shared/attack_payload.hpp"

class DMCheckbox;
class DMTextBox;
class FloatSliderWidget;

namespace devmode::room_config {

class AttackPayloadEditor {
public:
    using PayloadChangedCallback = std::function<void(const animation_update::AttackPayload&)>;

    AttackPayloadEditor();
    ~AttackPayloadEditor();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();

    void set_payload(const animation_update::AttackPayload& payload);
    void clear_payload();
    bool has_payload() const { return has_payload_; }

    void set_payload_changed_callback(PayloadChangedCallback callback);

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    void update_layout() const;
    void apply_payload_to_widgets();
    animation_update::AttackPayload payload_from_widgets() const;
    void emit_payload_if_changed();
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);

private:
    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;
    bool panel_bounds_override_active_ = false;
    SDL_Rect panel_bounds_override_{0, 0, 0, 0};

    mutable bool layout_dirty_ = true;
    mutable SDL_Rect panel_rect_{344, 56, 332, 620};
    mutable SDL_Rect title_rect_{0, 0, 0, 0};
    mutable SDL_Rect subtitle_rect_{0, 0, 0, 0};
    mutable SDL_Rect empty_hint_rect_{0, 0, 0, 0};

    std::unique_ptr<DMTextBox> damage_textbox_;
    std::unique_ptr<DMTextBox> damage_type_textbox_;
    std::unique_ptr<DMCheckbox> hitback_enabled_checkbox_;
    std::unique_ptr<DMTextBox> hitback_distance_textbox_;
    std::unique_ptr<DMTextBox> stun_frames_textbox_;
    std::unique_ptr<DMTextBox> animation_trigger_textbox_;
    std::unique_ptr<DMTextBox> sound_effect_textbox_;
    std::unique_ptr<DMTextBox> status_effects_textbox_;
    std::unique_ptr<DMTextBox> critical_hit_chance_textbox_;
    std::unique_ptr<DMTextBox> element_type_textbox_;
    std::unique_ptr<DMTextBox> recharge_seconds_textbox_;
    std::unique_ptr<FloatSliderWidget> recharge_random_weight_slider_;

    bool has_payload_ = false;
    bool applying_payload_to_widgets_ = false;
    animation_update::AttackPayload payload_{};
    PayloadChangedCallback on_payload_changed_{};
};

} // namespace devmode::room_config
