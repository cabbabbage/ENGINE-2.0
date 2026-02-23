#include "dev_footer_bar.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"

#include "draw_utils.hpp"
#include "utils/input.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kDefaultFooterHeight = 40;
constexpr int kFooterHorizontalPadding = 20;
constexpr int kFooterVerticalPadding = 6;
constexpr int kFooterGroupGap = 18;
constexpr int kFooterButtonSpacing = 12;
constexpr int kFooterButtonMinWidth = 110;
constexpr int kFooterHideButtonWidth = 32;
constexpr Uint64 kFooterSlideDurationMs = 72;
constexpr Uint64 kFooterZoneDebounceMs = 36;
constexpr float kFooterShowZoneRatio = 0.95f;
constexpr float kFooterUnlockZoneRatio = 0.80f;

const DMButtonStyle* button_style_for(const DevFooterBar::Button& btn) {
    if (btn.active) {
        if (btn.active_style_override) {
            return btn.active_style_override;
        }
        if (btn.style_override) {
            return btn.style_override;
        }
        return &DMStyles::AccentButton();
    }
    if (btn.style_override) {
        return btn.style_override;
    }
    return &DMStyles::HeaderButton();
}

void draw_label(SDL_Renderer* renderer, const std::string& text, int x, int y) {
    if (!renderer) return;
    const DMLabelStyle& style = DMStyles::Label();
    TTF_Font* font = style.open_font();
    if (!font) return;
    SDL_Surface* surf = ttf_util::RenderTextBlended(font, text.c_str(), style.color);
    if (!surf) {
        TTF_CloseFont(font);
        return;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
        SDL_Rect dst{x, y, surf->w, surf->h};
        sdl_render::Texture(renderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_DestroySurface(surf);
    TTF_CloseFont(font);
}

float smoothstep(float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - (2.0f * clamped));
}

}

DevFooterBar::DevFooterBar(std::string title)
    : title_(std::move(title)),
      height_(kDefaultFooterHeight) {
    depth_effects_checkbox_ = std::make_unique<DMCheckbox>("Depth Effects", false);
    movement_debug_checkbox_ = std::make_unique<DMCheckbox>("Movement Debug", movement_debug_enabled_);
    grid_checkbox_ = std::make_unique<DMCheckbox>("Show Grid", grid_overlay_enabled_);
    grid_stepper_ = std::make_unique<DMNumericStepper>("Grid Overlay (r)", 0, 10, grid_resolution_);
    hide_button_ = std::make_unique<DMButton>("v", &DMStyles::HeaderButton(), kFooterHideButtonWidth, DMButton::height());
}

void DevFooterBar::set_bounds(int width, int height) {
    screen_w_ = width;
    screen_h_ = height;
    layout();
}

void DevFooterBar::set_height(int height) {
    const int clamped = std::max(height, kDefaultFooterHeight);
    if (clamped == height_) {
        return;
    }
    height_ = clamped;
    layout();
}

void DevFooterBar::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    if (!visible_) {
        slide_active_ = false;
        debounce_pending_ = false;
        apply_rect_y(hidden_y());
        return;
    }
    apply_rect_y(auto_hidden_ ? hidden_y() : shown_y());
}

void DevFooterBar::set_title(const std::string& title) {
    if (title_ == title) return;
    title_ = title;
    layout();
}

void DevFooterBar::set_title_visible(bool visible) {
    if (show_title_ == visible) return;
    show_title_ = visible;
    layout();
}

void DevFooterBar::set_settings_controls_visible(bool visible) {
    if (settings_controls_visible_ == visible) {
        return;
    }
    settings_controls_visible_ = visible;
    layout();
}

void DevFooterBar::set_buttons(std::vector<Button> buttons) {
    buttons_ = std::move(buttons);
    for (auto& btn : buttons_) {
        const DMButtonStyle* style = button_style_for(btn);
        btn.widget = std::make_unique<DMButton>(btn.label, style, 120, DMButton::height());
    }
    layout_content();
}

void DevFooterBar::activate_button(const std::string& id) {
    for (auto& btn : buttons_) {
        const bool new_state = (btn.id == id);
        if (btn.active != new_state) {
            btn.active = new_state;
            if (btn.widget) {
                btn.widget->set_style(button_style_for(btn));
            }
            if (btn.on_toggle) {
                btn.on_toggle(btn.active);
            }
        }
    }
}

void DevFooterBar::set_active_button(const std::string& id, bool trigger_callback) {
    for (auto& btn : buttons_) {
        const bool should_activate = (btn.id == id);
        if (btn.momentary) {
            continue;
        }
        if (btn.active != should_activate) {
            btn.active = should_activate;
            if (btn.widget) {
                btn.widget->set_style(button_style_for(btn));
            }
            if (trigger_callback && btn.on_toggle) {
                btn.on_toggle(btn.active);
            }
        } else if (should_activate && trigger_callback && btn.on_toggle) {
            btn.on_toggle(btn.active);
        }
    }
    if (!trigger_callback) {
        return;
    }
    for (auto& btn : buttons_) {
        if (btn.momentary && btn.id == id && btn.on_toggle) {
            btn.on_toggle(true);
            btn.active = false;
            if (btn.widget) {
                btn.widget->set_style(button_style_for(btn));
            }
        } else if (!btn.momentary && btn.id != id && btn.active) {
            btn.active = false;
            if (btn.widget) {
                btn.widget->set_style(button_style_for(btn));
            }
            if (btn.on_toggle) {
                btn.on_toggle(false);
            }
        }
    }
}

void DevFooterBar::set_button_active_state(const std::string& id, bool active) {
    for (auto& btn : buttons_) {
        if (btn.id == id) {
            bool new_state = active;
            if (btn.momentary && active) {
                new_state = false;
            }
            if (btn.active != new_state) {
                btn.active = new_state;
                if (btn.widget) {
                    btn.widget->set_style(button_style_for(btn));
                }
            }
        }
    }
}

void DevFooterBar::update(const Input& input) {
    if (!visible_ || screen_h_ <= 0) {
        return;
    }

    const Uint64 now_ms = SDL_GetTicks();
    const float cursor_ratio = static_cast<float>(input.getY()) / static_cast<float>(std::max(1, screen_h_));
    const bool in_show_zone = cursor_ratio >= kFooterShowZoneRatio;
    const bool above_unlock_zone = cursor_ratio < kFooterUnlockZoneRatio;

    if (manual_hidden_lock_) {
        if (above_unlock_zone) {
            manual_hidden_lock_ = false;
        } else {
            request_hidden_state(true, now_ms, true);
        }
    }

    if (!manual_hidden_lock_) {
        const bool should_hide = !in_show_zone;
        request_hidden_state(should_hide, now_ms, false);
    }

    update_slide(now_ms);
}

bool DevFooterBar::handle_event(const SDL_Event& e) {
    if (!visible_ || !input_enabled_) return false;

    const bool pointer_event =
        (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION);
    const bool wheel_event = (e.type == SDL_EVENT_MOUSE_WHEEL);

    SDL_Point pointer{0, 0};
    if (pointer_event) {
        pointer.x = (e.type == SDL_EVENT_MOUSE_MOTION) ? e.motion.x : e.button.x;
        pointer.y = (e.type == SDL_EVENT_MOUSE_MOTION) ? e.motion.y : e.button.y;
    } else if (wheel_event) {
        sdl_mouse_util::GetMouseState(&pointer.x, &pointer.y);
    }

    const bool in_footer = (pointer_event || wheel_event) && SDL_PointInRect(&pointer, &rect_);

    bool used = false;

    if (hide_button_ && hide_button_->handle_event(e)) {
        used = true;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            manual_hidden_lock_ = true;
            request_hidden_state(true, SDL_GetTicks(), true);
        }
    }

    if (settings_controls_visible_) {
        if (depth_effects_checkbox_ && depth_effects_checkbox_->handle_event(e)) {
            used = true;
            if (on_depth_effects_toggle_) {
                on_depth_effects_toggle_(depth_effects_checkbox_->value());
            }
        }

        if (movement_debug_checkbox_ && movement_debug_checkbox_->handle_event(e)) {
            used = true;
            movement_debug_enabled_ = movement_debug_checkbox_->value();
            if (on_movement_debug_toggle_) {
                on_movement_debug_toggle_(movement_debug_enabled_);
            }
        }

        if (grid_checkbox_ && grid_checkbox_->handle_event(e)) {
            used = true;
            grid_overlay_enabled_ = grid_checkbox_->value();
            if (on_grid_overlay_toggle_) {
                on_grid_overlay_toggle_(grid_overlay_enabled_);
            }
        }

        if (grid_stepper_ && grid_stepper_->handle_event(e)) {
            used = true;
            grid_resolution_ = grid_stepper_->value();
            if (on_grid_resolution_change_) {
                on_grid_resolution_change_(grid_resolution_, true);
            }
        }
    }

    for (auto& btn : buttons_) {
        if (!btn.widget) continue;
        if (btn.widget->handle_event(e)) {
            used = true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                if (btn.momentary) {
                    if (btn.on_toggle) btn.on_toggle(true);
                    btn.active = false;
                    if (btn.widget) {
                        btn.widget->set_style(button_style_for(btn));
                    }
                } else {
                    if (btn.active) {
                        btn.active = false;
                        if (btn.on_toggle) btn.on_toggle(false);
                        btn.widget->set_style(button_style_for(btn));
                    } else {
                        set_active_button(btn.id, true);
                    }
                }
            }
        }
    }

    if (used) {
        return true;
    }

    if (in_footer) {
        return true;
    }

    return false;
}

void DevFooterBar::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_Color top = DMStyles::PanelHeader();
    const SDL_Color bottom = dm_draw::DarkenColor(top, 0.25f);
    dm_draw::DrawRoundedGradientRect(renderer, rect_, DMStyles::CornerRadius(), top, bottom);
    dm_draw::DrawRoundedOutline(renderer, rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());

    SDL_Color highlight = DMStyles::HighlightColor();
    highlight.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(highlight.a * 0.35f), 0, 255));
    SDL_SetRenderDrawColor(renderer, highlight.r, highlight.g, highlight.b, highlight.a);
    SDL_RenderLine(renderer, rect_.x, rect_.y, rect_.x + rect_.w - 1, rect_.y);

    const bool draw_separator = settings_controls_visible_ && (grid_checkbox_ && grid_stepper_) && (title_bounds_.w > 0 || !buttons_.empty());
    if (draw_separator) {
        SDL_Color separator = DMStyles::Border();
        separator.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(separator.a * 0.8f), 0, 255));
        SDL_SetRenderDrawColor(renderer, separator.r, separator.g, separator.b, separator.a);
        const int separator_x = std::min(rect_.x + rect_.w - 1, grid_controls_right_ + kFooterGroupGap / 2);
        SDL_RenderLine(renderer, separator_x, rect_.y + kFooterVerticalPadding, separator_x, rect_.y + rect_.h - kFooterVerticalPadding);
    }

    if (settings_controls_visible_) {
        if (depth_effects_checkbox_) {
            depth_effects_checkbox_->render(renderer);
        }

        if (movement_debug_checkbox_) {
            movement_debug_checkbox_->render(renderer);
        }

        if (grid_checkbox_) {
            grid_checkbox_->render(renderer);
        }
        if (grid_stepper_) {
            grid_stepper_->render(renderer);
        }
    }

    if (hide_button_) {
        hide_button_->render(renderer);
    }

    if (title_bounds_.w > 0 && !title_.empty()) {
        int text_y = title_bounds_.y + (title_bounds_.h - DMStyles::Label().font_size) / 2;
        const int text_x = title_bounds_.x;
        draw_label(renderer, title_, text_x, text_y);
    }

    for (const auto& btn : buttons_) {
        if (!btn.widget) continue;
        btn.widget->render(renderer);
    }
}

const DevFooterBar::Button* DevFooterBar::find_button(const std::string& id) const {
    for (const auto& btn : buttons_) {
        if (btn.id == id) {
            return &btn;
        }
    }
    return nullptr;
}

std::optional<SDL_Rect> DevFooterBar::button_rect(const std::string& id) const {
    for (const auto& btn : buttons_) {
        if (btn.id != id) continue;
        if (!btn.widget) continue;
        SDL_Rect rect = btn.widget->rect();
        if (rect.w > 0 && rect.h > 0) {
            return rect;
        }
    }
    return std::nullopt;
}

bool DevFooterBar::contains(int x, int y) const {
    if (!visible_) return false;
    SDL_Point p{x, y};
    return SDL_PointInRect(&p, &rect_);
}

void DevFooterBar::layout() {
    rect_.w = screen_w_;
    rect_.h = height_;
    rect_.x = 0;

    const int visible_y = shown_y();
    const int fully_hidden_y = hidden_y();
    if (slide_active_) {
        slide_start_y_ = std::clamp(slide_start_y_, visible_y, fully_hidden_y);
        slide_target_y_ = std::clamp(slide_target_y_, visible_y, fully_hidden_y);
        rect_.y = std::clamp(rect_.y, visible_y, fully_hidden_y);
    } else {
        rect_.y = auto_hidden_ ? fully_hidden_y : visible_y;
    }

    update_title_width();
    layout_content();
}

void DevFooterBar::layout_content() {
    layout_hide_button();
    grid_controls_right_ = content_start_x();
    title_bounds_ = SDL_Rect{0, 0, 0, 0};
    layout_grid_controls();
    layout_title_region();
    layout_buttons();
}

void DevFooterBar::layout_hide_button() {
    hide_button_rect_ = SDL_Rect{0, 0, 0, 0};
    if (!hide_button_) {
        return;
    }

    const int x = rect_.x + kFooterHorizontalPadding;
    const int y = rect_.y + (rect_.h - DMButton::height()) / 2;
    hide_button_rect_ = SDL_Rect{x, y, kFooterHideButtonWidth, DMButton::height()};
    hide_button_->set_rect(hide_button_rect_);
    hide_button_rect_ = hide_button_->rect();
}

void DevFooterBar::layout_title_region() {
    title_bounds_ = SDL_Rect{0, 0, 0, 0};
    if (!show_title_ || title_width_ <= 0) {
        return;
    }

    int x = content_start_x();
    if (settings_controls_visible_ && grid_checkbox_ && grid_stepper_) {
        x = std::max(x, grid_controls_right_ + kFooterGroupGap);
    }

    const int max_width = std::max(0, rect_.w - (x - rect_.x) - kFooterHorizontalPadding);
    if (max_width <= 0) {
        return;
    }

    const int clamped_width = std::min(title_width_, max_width);
    title_bounds_ = SDL_Rect{x, rect_.y, clamped_width, rect_.h};
}

void DevFooterBar::layout_buttons() {
    int button_start = content_start_x();
    if (settings_controls_visible_ && grid_checkbox_ && grid_stepper_) {
        button_start = std::max(button_start, grid_controls_right_ + kFooterGroupGap);
    }
    if (title_bounds_.w > 0) {
        button_start = std::max(button_start, title_bounds_.x + title_bounds_.w + kFooterGroupGap);
    }

    const int right_limit = rect_.x + rect_.w - kFooterHorizontalPadding;
    const int span = right_limit - button_start;
    const int button_gap = kFooterButtonSpacing;

    if (span <= 0) {
        for (auto& btn : buttons_) {
            if (btn.widget) {
                btn.widget->set_rect(SDL_Rect{0, 0, 0, 0});
            }
        }
        return;
    }

    struct ButtonLayoutInfo {
        DMButton* widget = nullptr;
        int width = 0;
};

    std::vector<ButtonLayoutInfo> visible;
    visible.reserve(buttons_.size());
    int total_width = 0;
    int count = 0;
    bool out_of_space = false;

    for (auto& btn : buttons_) {
        if (!btn.widget) continue;

        if (out_of_space) {
            btn.widget->set_rect(SDL_Rect{0, 0, 0, 0});
            continue;
        }

        int width = std::max(btn.widget->preferred_width(), kFooterButtonMinWidth);

        const int projected_total = total_width + width;
        const int projected_count = count + 1;
        const int projected_block = projected_total + button_gap * std::max(0, projected_count - 1);

        if (projected_block > span) {
            btn.widget->set_rect(SDL_Rect{0, 0, 0, 0});
            out_of_space = true;
            continue;
        }

        visible.push_back({btn.widget.get(), width});
        total_width = projected_total;
        count = projected_count;
    }

    if (visible.empty()) {
        return;
    }

    const int y = rect_.y + (rect_.h - DMButton::height()) / 2;
    const int block_width = total_width + button_gap * std::max(0, count - 1);
    int current_x = std::max(button_start, right_limit - block_width);

    for (size_t i = 0; i < visible.size(); ++i) {
        auto& info = visible[i];
        info.widget->set_rect(SDL_Rect{current_x, y, info.width, DMButton::height()});
        current_x += info.width;
        if (i + 1 < visible.size()) {
            current_x += button_gap;
        }
    }
}

int DevFooterBar::content_start_x() const {
    int start = rect_.x + kFooterHorizontalPadding;
    if (hide_button_) {
        start = std::max(start, hide_button_rect_.x + hide_button_rect_.w + kFooterGroupGap);
    }
    return start;
}

int DevFooterBar::shown_y() const {
    return std::max(0, screen_h_ - rect_.h);
}

int DevFooterBar::hidden_y() const {
    return std::max(0, screen_h_);
}

void DevFooterBar::apply_rect_y(int y) {
    rect_.y = y;
    layout_content();
}

void DevFooterBar::begin_slide(bool hidden, Uint64 now_ms) {
    auto_hidden_ = hidden;
    const int target_y = hidden ? hidden_y() : shown_y();
    if (rect_.y == target_y) {
        slide_active_ = false;
        return;
    }
    slide_active_ = true;
    slide_start_y_ = rect_.y;
    slide_target_y_ = target_y;
    slide_started_ms_ = now_ms;
}

void DevFooterBar::update_slide(Uint64 now_ms) {
    if (!slide_active_) {
        return;
    }

    const Uint64 elapsed = now_ms - slide_started_ms_;
    if (elapsed >= kFooterSlideDurationMs) {
        slide_active_ = false;
        if (rect_.y != slide_target_y_) {
            apply_rect_y(slide_target_y_);
        }
        return;
    }

    const float t = static_cast<float>(elapsed) / static_cast<float>(kFooterSlideDurationMs);
    const float eased = smoothstep(t);
    const float y = static_cast<float>(slide_start_y_) +
        (static_cast<float>(slide_target_y_ - slide_start_y_) * eased);
    const int next_y = static_cast<int>(std::lround(y));
    if (next_y != rect_.y) {
        apply_rect_y(next_y);
    }
}

void DevFooterBar::request_hidden_state(bool hidden, Uint64 now_ms, bool bypass_debounce) {
    if (hidden == auto_hidden_) {
        debounce_pending_ = false;
        return;
    }

    if (bypass_debounce) {
        debounce_pending_ = false;
        begin_slide(hidden, now_ms);
        return;
    }

    if (!debounce_pending_ || debounce_hidden_target_ != hidden) {
        debounce_pending_ = true;
        debounce_hidden_target_ = hidden;
        debounce_started_ms_ = now_ms;
        return;
    }

    if (now_ms - debounce_started_ms_ >= kFooterZoneDebounceMs) {
        debounce_pending_ = false;
        begin_slide(hidden, now_ms);
    }
}

void DevFooterBar::update_title_width() {
    title_width_ = 0;
    if (!show_title_ || title_.empty()) {
        return;
    }
    const DMLabelStyle& style = DMStyles::Label();
    TTF_Font* font = style.open_font();
    if (!font) return;
    int w = 0;
    int h = 0;
    if (ttf_util::GetStringSize(font, title_, &w, &h)) {
        title_width_ = w;
    }
    TTF_CloseFont(font);
}

void DevFooterBar::set_grid_overlay_enabled(bool enabled) {
    if (grid_overlay_enabled_ != enabled) {
        grid_overlay_enabled_ = enabled;
        if (grid_checkbox_) {
            grid_checkbox_->set_value(enabled);
        }
        if (on_grid_overlay_toggle_) {
            on_grid_overlay_toggle_(enabled);
        }
    }
}

void DevFooterBar::set_grid_resolution(int resolution) {
    if (grid_resolution_ != resolution) {
        grid_resolution_ = resolution;
        if (grid_stepper_) {
            grid_stepper_->set_value(resolution);
        }
        if (on_grid_resolution_change_) {
            on_grid_resolution_change_(resolution, false);
        }
    }
}

void DevFooterBar::set_grid_controls_callbacks(std::function<void(bool)> on_overlay_toggle,
                                               std::function<void(int, bool)> on_resolution_change) {
    on_grid_overlay_toggle_ = std::move(on_overlay_toggle);
    on_grid_resolution_change_ = std::move(on_resolution_change);
}

void DevFooterBar::set_movement_debug_enabled(bool enabled) {
    if (movement_debug_enabled_ == enabled) {
        return;
    }
    movement_debug_enabled_ = enabled;
    if (movement_debug_checkbox_) {
        movement_debug_checkbox_->set_value(enabled);
    }
    if (on_movement_debug_toggle_) {
        on_movement_debug_toggle_(enabled);
    }
}

void DevFooterBar::set_movement_debug_callback(std::function<void(bool)> cb) {
    on_movement_debug_toggle_ = std::move(cb);
}

void DevFooterBar::layout_grid_controls() {
    grid_controls_right_ = content_start_x();
    if (!settings_controls_visible_) {
        if (depth_effects_checkbox_) depth_effects_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (movement_debug_checkbox_) movement_debug_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (grid_checkbox_) grid_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (grid_stepper_) grid_stepper_->set_rect(SDL_Rect{0, 0, 0, 0});
        return;
    }
    if (!depth_effects_checkbox_ || !movement_debug_checkbox_ || !grid_checkbox_ || !grid_stepper_) {
        return;
    }

    int x = content_start_x();
    const int checkbox_y = rect_.y + (rect_.h - DMCheckbox::height()) / 2;
    const int stepper_y = rect_.y + (rect_.h - DMNumericStepper::height()) / 2;
    const int gap = DMSpacing::small_gap();

    SDL_Rect depth_rect{x, checkbox_y, depth_effects_checkbox_->preferred_width(), DMCheckbox::height()};
    depth_effects_checkbox_->set_rect(depth_rect);
    x += depth_rect.w + gap;

    SDL_Rect movement_rect{x, checkbox_y, movement_debug_checkbox_->preferred_width(), DMCheckbox::height()};
    movement_debug_checkbox_->set_rect(movement_rect);
    x += movement_rect.w + gap;

    SDL_Rect checkbox_rect{x, checkbox_y, grid_checkbox_->preferred_width(), DMCheckbox::height()};
    grid_checkbox_->set_rect(checkbox_rect);
    x += checkbox_rect.w + gap;

    constexpr int kStepperWidth = 180;
    SDL_Rect stepper_rect{x, stepper_y, kStepperWidth, DMNumericStepper::height()};
    grid_stepper_->set_rect(stepper_rect);
    grid_controls_right_ = stepper_rect.x + stepper_rect.w;
}

void DevFooterBar::set_depth_effects_enabled(bool enabled) {
    if (depth_effects_checkbox_) {
        depth_effects_checkbox_->set_value(enabled);
    }
}

void DevFooterBar::set_depth_effects_callbacks(std::function<void(bool)> cb) {
    on_depth_effects_toggle_ = std::move(cb);
}




