#include "config/room_config/attack_payload_editor.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>

#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/float_slider_widget.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/widgets.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"

namespace devmode::room_config {

namespace {

constexpr int kPanelMargin = 344;
constexpr int kTopOffset = 56;
constexpr int kPanelWidth = 332;
constexpr int kPanelMinHeight = 420;
constexpr int kPanelPadding = 12;
constexpr int kSectionGap = 10;
constexpr int kRowGap = 8;
constexpr int kHeaderHeight = 22;

std::string trim_copy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

int parse_int_or(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

float parse_float_or(const std::string& text, float fallback) {
    try {
        return std::stof(text);
    } catch (...) {
        return fallback;
    }
}

std::vector<std::string> parse_csv(const std::string& csv) {
    std::vector<std::string> values;
    std::size_t cursor = 0;
    while (cursor <= csv.size()) {
        const std::size_t comma = csv.find(',', cursor);
        const std::string token = trim_copy(csv.substr(cursor, comma - cursor));
        if (!token.empty()) {
            values.push_back(token);
        }
        if (comma == std::string::npos) {
            break;
        }
        cursor = comma + 1;
    }
    return values;
}

std::string join_csv(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += values[i];
    }
    return out;
}

bool payloads_equal(const animation_update::AttackPayload& lhs,
                    const animation_update::AttackPayload& rhs) {
    return lhs.damage_amount == rhs.damage_amount &&
           lhs.damage_type == rhs.damage_type &&
           lhs.hitback_enabled == rhs.hitback_enabled &&
           std::fabs(lhs.hitback_distance - rhs.hitback_distance) <= 1e-5f &&
           lhs.stun_frames == rhs.stun_frames &&
           lhs.animation_trigger == rhs.animation_trigger &&
           lhs.sound_effect == rhs.sound_effect &&
           lhs.status_effects == rhs.status_effects &&
           std::fabs(lhs.critical_hit_chance - rhs.critical_hit_chance) <= 1e-5f &&
           lhs.element_type == rhs.element_type &&
           std::fabs(lhs.recharge_seconds - rhs.recharge_seconds) <= 1e-5f &&
           std::fabs(lhs.recharge_random_weight - rhs.recharge_random_weight) <= 1e-5f &&
           lhs.payload_id == rhs.payload_id;
}

} // namespace

AttackPayloadEditor::AttackPayloadEditor() {
    damage_textbox_ = std::make_unique<DMTextBox>("Damage Amount", "0");
    damage_type_textbox_ = std::make_unique<DMTextBox>("Damage Type", "physical");
    hitback_enabled_checkbox_ = std::make_unique<DMCheckbox>("Enable Hitback", true);
    hitback_distance_textbox_ = std::make_unique<DMTextBox>("Hitback Distance", "0");
    stun_frames_textbox_ = std::make_unique<DMTextBox>("Stun Frames", "0");
    animation_trigger_textbox_ = std::make_unique<DMTextBox>("Animation Trigger", "");
    sound_effect_textbox_ = std::make_unique<DMTextBox>("Sound Effect", "");
    status_effects_textbox_ = std::make_unique<DMTextBox>("Status Effects (CSV)", "");
    critical_hit_chance_textbox_ = std::make_unique<DMTextBox>("Critical Hit Chance (0-1)", "0");
    element_type_textbox_ = std::make_unique<DMTextBox>("Element Type", "none");
    recharge_seconds_textbox_ = std::make_unique<DMTextBox>("Recharge Seconds", "0");
    recharge_random_weight_slider_ =
        std::make_unique<FloatSliderWidget>("Recharge Random Weight", 0.05f, 5.0f, 0.05f, 1.0f, 2);
}

AttackPayloadEditor::~AttackPayloadEditor() = default;

void AttackPayloadEditor::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    layout_dirty_ = true;
}

void AttackPayloadEditor::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void AttackPayloadEditor::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_bounds_override_ = bounds;
    panel_bounds_override_active_ = bounds.w > 0 && bounds.h > 0;
    layout_dirty_ = true;
}

void AttackPayloadEditor::clear_panel_bounds_override() {
    panel_bounds_override_active_ = false;
    panel_bounds_override_ = SDL_Rect{0, 0, 0, 0};
    layout_dirty_ = true;
}

void AttackPayloadEditor::set_payload(const animation_update::AttackPayload& payload) {
    has_payload_ = true;
    payload_ = animation_update::sanitize_attack_payload(payload);
    apply_payload_to_widgets();
    layout_dirty_ = true;
}

void AttackPayloadEditor::clear_payload() {
    if (!has_payload_) {
        return;
    }
    has_payload_ = false;
    payload_ = animation_update::make_default_attack_payload();
    layout_dirty_ = true;
}

void AttackPayloadEditor::set_payload_changed_callback(PayloadChangedCallback callback) {
    on_payload_changed_ = std::move(callback);
}

void AttackPayloadEditor::apply_payload_to_widgets() {
    applying_payload_to_widgets_ = true;
    if (damage_textbox_) {
        damage_textbox_->set_value(std::to_string(payload_.damage_amount));
    }
    if (damage_type_textbox_) {
        damage_type_textbox_->set_value(payload_.damage_type);
    }
    if (hitback_enabled_checkbox_) {
        hitback_enabled_checkbox_->set_value(payload_.hitback_enabled);
    }
    if (hitback_distance_textbox_) {
        hitback_distance_textbox_->set_value(std::to_string(payload_.hitback_distance));
    }
    if (stun_frames_textbox_) {
        stun_frames_textbox_->set_value(std::to_string(payload_.stun_frames));
    }
    if (animation_trigger_textbox_) {
        animation_trigger_textbox_->set_value(payload_.animation_trigger);
    }
    if (sound_effect_textbox_) {
        sound_effect_textbox_->set_value(payload_.sound_effect);
    }
    if (status_effects_textbox_) {
        status_effects_textbox_->set_value(join_csv(payload_.status_effects));
    }
    if (critical_hit_chance_textbox_) {
        critical_hit_chance_textbox_->set_value(std::to_string(payload_.critical_hit_chance));
    }
    if (element_type_textbox_) {
        element_type_textbox_->set_value(payload_.element_type);
    }
    if (recharge_seconds_textbox_) {
        recharge_seconds_textbox_->set_value(std::to_string(payload_.recharge_seconds));
    }
    if (recharge_random_weight_slider_) {
        recharge_random_weight_slider_->set_value(payload_.recharge_random_weight);
    }
    applying_payload_to_widgets_ = false;
}

animation_update::AttackPayload AttackPayloadEditor::payload_from_widgets() const {
    animation_update::AttackPayload payload = payload_;
    payload.damage_amount = parse_int_or(damage_textbox_ ? damage_textbox_->value() : "0", payload.damage_amount);
    payload.damage_type = damage_type_textbox_ ? damage_type_textbox_->value() : payload.damage_type;
    payload.hitback_enabled = hitback_enabled_checkbox_ ? hitback_enabled_checkbox_->value() : payload.hitback_enabled;
    payload.hitback_distance =
        parse_float_or(hitback_distance_textbox_ ? hitback_distance_textbox_->value() : "0", payload.hitback_distance);
    payload.stun_frames = parse_int_or(stun_frames_textbox_ ? stun_frames_textbox_->value() : "0", payload.stun_frames);
    payload.animation_trigger = animation_trigger_textbox_ ? animation_trigger_textbox_->value() : payload.animation_trigger;
    payload.sound_effect = sound_effect_textbox_ ? sound_effect_textbox_->value() : payload.sound_effect;
    payload.status_effects = parse_csv(status_effects_textbox_ ? status_effects_textbox_->value() : std::string{});
    payload.critical_hit_chance =
        parse_float_or(critical_hit_chance_textbox_ ? critical_hit_chance_textbox_->value() : "0", payload.critical_hit_chance);
    payload.element_type = element_type_textbox_ ? element_type_textbox_->value() : payload.element_type;
    payload.recharge_seconds =
        parse_float_or(recharge_seconds_textbox_ ? recharge_seconds_textbox_->value() : "0", payload.recharge_seconds);
    payload.recharge_random_weight = recharge_random_weight_slider_ ? recharge_random_weight_slider_->value() : payload.recharge_random_weight;
    return animation_update::sanitize_attack_payload(std::move(payload));
}

void AttackPayloadEditor::emit_payload_if_changed() {
    if (applying_payload_to_widgets_ || !has_payload_) {
        return;
    }
    animation_update::AttackPayload candidate = payload_from_widgets();
    if (payloads_equal(candidate, payload_)) {
        return;
    }
    payload_ = candidate;
    if (on_payload_changed_) {
        on_payload_changed_(payload_);
    }
}

bool AttackPayloadEditor::handle_event(const SDL_Event& event) {
    if (!visible_) {
        return false;
    }

    update_layout();

    bool handled = false;
    bool changed = false;
    if (has_payload_) {
        auto handle_text = [&](DMTextBox* textbox) {
            if (!textbox) {
                return;
            }
            if (textbox->handle_event(event)) {
                handled = true;
                changed = true;
            }
        };
        handle_text(damage_textbox_.get());
        handle_text(damage_type_textbox_.get());
        handle_text(hitback_distance_textbox_.get());
        handle_text(stun_frames_textbox_.get());
        handle_text(animation_trigger_textbox_.get());
        handle_text(sound_effect_textbox_.get());
        handle_text(status_effects_textbox_.get());
        handle_text(critical_hit_chance_textbox_.get());
        handle_text(element_type_textbox_.get());
        handle_text(recharge_seconds_textbox_.get());
        if (recharge_random_weight_slider_ && recharge_random_weight_slider_->handle_event(event)) {
            handled = true;
            changed = true;
        }

        if (hitback_enabled_checkbox_) {
            const bool before = hitback_enabled_checkbox_->value();
            if (hitback_enabled_checkbox_->handle_event(event)) {
                handled = true;
                changed = changed || (before != hitback_enabled_checkbox_->value());
            }
        }
    }

    if (changed) {
        emit_payload_if_changed();
    }

    const bool payload_text_editing =
        (damage_textbox_ && damage_textbox_->is_editing()) ||
        (damage_type_textbox_ && damage_type_textbox_->is_editing()) ||
        (hitback_distance_textbox_ && hitback_distance_textbox_->is_editing()) ||
        (stun_frames_textbox_ && stun_frames_textbox_->is_editing()) ||
        (animation_trigger_textbox_ && animation_trigger_textbox_->is_editing()) ||
        (sound_effect_textbox_ && sound_effect_textbox_->is_editing()) ||
        (status_effects_textbox_ && status_effects_textbox_->is_editing()) ||
        (critical_hit_chance_textbox_ && critical_hit_chance_textbox_->is_editing()) ||
        (element_type_textbox_ && element_type_textbox_->is_editing()) ||
        (recharge_seconds_textbox_ && recharge_seconds_textbox_->is_editing());
    if ((event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN) && payload_text_editing) {
        handled = true;
    }

    SDL_Point pointer{0, 0};
    const bool pointer_event =
        event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    const bool wheel_event = event.type == SDL_EVENT_MOUSE_WHEEL;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        pointer = sdl_mouse_util::MotionPoint(event.motion);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        pointer = sdl_mouse_util::ButtonPoint(event.button);
    } else if (wheel_event) {
        sdl_mouse_util::GetMouseState(&pointer.x, &pointer.y);
    }
    if ((pointer_event || wheel_event) && point_in_rect(pointer.x, pointer.y, panel_rect_)) {
        handled = true;
    }
    return handled;
}

void AttackPayloadEditor::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) {
        return;
    }

    update_layout();

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect(renderer,
                             panel_rect_,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, panel_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());

    const DMLabelStyle& label_style = DMStyles::Label();
    DMFontCache::instance().draw_text(renderer, label_style, "Attack Payload", title_rect_.x, title_rect_.y);
    if (!has_payload_) {
        DMFontCache::instance().draw_text(renderer,
                                          label_style,
                                          "Select an attack box to edit payload data.",
                                          empty_hint_rect_.x,
                                          empty_hint_rect_.y);
        return;
    }

    const std::string subtitle = payload_.payload_id.empty()
        ? std::string{"Payload ID: <none>"}
        : std::string{"Payload ID: "} + payload_.payload_id;
    DMFontCache::instance().draw_text(renderer, label_style, subtitle, subtitle_rect_.x, subtitle_rect_.y);

    if (damage_textbox_) damage_textbox_->render(renderer);
    if (damage_type_textbox_) damage_type_textbox_->render(renderer);
    if (hitback_enabled_checkbox_) hitback_enabled_checkbox_->render(renderer);
    if (hitback_distance_textbox_) hitback_distance_textbox_->render(renderer);
    if (stun_frames_textbox_) stun_frames_textbox_->render(renderer);
    if (animation_trigger_textbox_) animation_trigger_textbox_->render(renderer);
    if (sound_effect_textbox_) sound_effect_textbox_->render(renderer);
    if (status_effects_textbox_) status_effects_textbox_->render(renderer);
    if (critical_hit_chance_textbox_) critical_hit_chance_textbox_->render(renderer);
    if (element_type_textbox_) element_type_textbox_->render(renderer);
    if (recharge_seconds_textbox_) recharge_seconds_textbox_->render(renderer);
    if (recharge_random_weight_slider_) recharge_random_weight_slider_->render(renderer);
}

bool AttackPayloadEditor::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    update_layout();
    return point_in_rect(x, y, panel_rect_);
}

void AttackPayloadEditor::update_layout() const {
    if (!layout_dirty_) {
        return;
    }

    if (panel_bounds_override_active_) {
        panel_rect_ = panel_bounds_override_;
    } else {
        panel_rect_ = SDL_Rect{kPanelMargin, kTopOffset, kPanelWidth, std::max(kPanelMinHeight, screen_h_ - (kTopOffset + 24))};
    }
    panel_rect_.w = std::max(panel_rect_.w, 240);
    panel_rect_.h = std::max(panel_rect_.h, kPanelMinHeight);

    const int content_x = panel_rect_.x + kPanelPadding;
    const int content_w = std::max(0, panel_rect_.w - (kPanelPadding * 2));

    int y = panel_rect_.y + kPanelPadding;
    title_rect_ = SDL_Rect{content_x, y, content_w, kHeaderHeight};
    y += kHeaderHeight + 2;
    subtitle_rect_ = SDL_Rect{content_x, y, content_w, kHeaderHeight};
    y += kHeaderHeight + kSectionGap;
    empty_hint_rect_ = SDL_Rect{content_x, y, content_w, kHeaderHeight};

    auto layout_textbox = [&](DMTextBox* textbox) {
        if (!textbox) {
            return;
        }
        const int height = textbox->preferred_height(content_w);
        textbox->set_rect(SDL_Rect{content_x, y, content_w, height});
        y += height + kRowGap;
    };

    if (has_payload_) {
        layout_textbox(damage_textbox_.get());
        layout_textbox(damage_type_textbox_.get());
        if (hitback_enabled_checkbox_) {
            hitback_enabled_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()});
            y += DMCheckbox::height() + kRowGap;
        }
        layout_textbox(hitback_distance_textbox_.get());
        layout_textbox(stun_frames_textbox_.get());
        layout_textbox(animation_trigger_textbox_.get());
        layout_textbox(sound_effect_textbox_.get());
        layout_textbox(status_effects_textbox_.get());
        layout_textbox(critical_hit_chance_textbox_.get());
        layout_textbox(element_type_textbox_.get());
        layout_textbox(recharge_seconds_textbox_.get());
        if (recharge_random_weight_slider_) {
            const int height = recharge_random_weight_slider_->height_for_width(content_w);
            recharge_random_weight_slider_->set_rect(SDL_Rect{content_x, y, content_w, height});
            y += height + kRowGap;
        }
    } else {
        auto hide_textbox = [](DMTextBox* textbox) {
            if (textbox) {
                textbox->set_rect(SDL_Rect{0, 0, 0, 0});
            }
        };
        hide_textbox(damage_textbox_.get());
        hide_textbox(damage_type_textbox_.get());
        hide_textbox(hitback_distance_textbox_.get());
        hide_textbox(stun_frames_textbox_.get());
        hide_textbox(animation_trigger_textbox_.get());
        hide_textbox(sound_effect_textbox_.get());
        hide_textbox(status_effects_textbox_.get());
        hide_textbox(critical_hit_chance_textbox_.get());
        hide_textbox(element_type_textbox_.get());
        hide_textbox(recharge_seconds_textbox_.get());
        if (recharge_random_weight_slider_) {
            recharge_random_weight_slider_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (hitback_enabled_checkbox_) {
            hitback_enabled_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
    }

    layout_dirty_ = false;
}

bool AttackPayloadEditor::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point p{x, y};
    return SDL_PointInRect(&p, &rect);
}

} // namespace devmode::room_config
