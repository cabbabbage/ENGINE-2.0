#include "dev_footer_bar.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"

#include "draw_utils.hpp"
#include "font_cache.hpp"
#include "utils/input.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr int kDefaultFooterHeight = 48;
constexpr int kFooterHorizontalPadding = 24;
constexpr int kFooterVerticalPadding = 8;
constexpr int kFooterGroupGap = 24;
constexpr int kFooterButtonSpacing = 14;
constexpr int kFooterButtonMinWidth = 110;
constexpr int kFooterHideButtonWidth = 32;
constexpr int kEditorTabGap = 8;
constexpr int kEditorFrameRowGap = 6;
constexpr int kEditorFrameStripHeight = 46;
constexpr int kEditorFrameChipWidth = 56;
constexpr int kEditorFrameChipGap = 8;
constexpr int kEditorFrameNavButtonWidth = 56;
constexpr int kEditorAnimationLabelHeight = 20;
constexpr Uint64 kFooterSlideDurationMs = 88;
constexpr Uint64 kFooterZoneDebounceMs = 36;
constexpr float kFooterShowZoneRatio = 0.90f;
constexpr float kFooterUnlockZoneRatio = 0.80f;
constexpr SDL_Color kEditorFrameSelectedColor{255, 140, 0, 236};

const DMButtonStyle* default_group_style(FooterButtonGroup group) {
    switch (group) {
        case FooterButtonGroup::Primary:
            return &DMStyles::HeaderButton();
        case FooterButtonGroup::Panels:
            return &DMStyles::ListButton();
        case FooterButtonGroup::Actions:
            return &DMStyles::SecondaryButton();
        case FooterButtonGroup::Utilities:
            return &DMStyles::SecondaryButton();
        default:
            return &DMStyles::HeaderButton();
    }
}

const DMButtonStyle* default_group_active_style(FooterButtonGroup group) {
    switch (group) {
        case FooterButtonGroup::Primary:
        case FooterButtonGroup::Panels:
        case FooterButtonGroup::Actions:
        case FooterButtonGroup::Utilities:
            return &DMStyles::AccentButton();
        default:
            return &DMStyles::AccentButton();
    }
}

const DMButtonStyle* button_style_for(const DevFooterBar::Button& btn) {
    const DMButtonStyle* base_style = btn.style_override ? btn.style_override : default_group_style(btn.group);
    const DMButtonStyle* active_style =
        btn.active_style_override ? btn.active_style_override : default_group_active_style(btn.group);
    if (btn.active) {
        return active_style ? active_style : base_style;
    }
    return base_style ? base_style : &DMStyles::HeaderButton();
}

const DMButtonStyle* editor_tab_style_for(const DevFooterBar::EditorTab& tab) {
    if (!tab.enabled) {
        return &DMStyles::SecondaryButton();
    }
    return tab.active ? &DMStyles::AccentButton() : &DMStyles::ListButton();
}

const DMButtonStyle* editor_nav_button_style(bool enabled) {
    return enabled ? &DMStyles::SecondaryButton() : &DMStyles::ListButton();
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

void DevFooterBar::set_editor_navigation_enabled(bool enabled) {
    if (editor_navigation_enabled_ == enabled) {
        return;
    }
    editor_navigation_enabled_ = enabled;
    if (!editor_navigation_enabled_) {
        editor_hovered_frame_index_ = -1;
        editor_pressed_frame_index_ = -1;
        editor_frame_scroll_offset_ = 0.0f;
    }
    layout_content();
}

void DevFooterBar::set_editor_tabs(std::vector<EditorTab> tabs) {
    editor_tabs_ = std::move(tabs);
    for (auto& tab : editor_tabs_) {
        tab.widget = std::make_unique<DMButton>(tab.label, editor_tab_style_for(tab), 120, DMButton::height());
    }
    layout_content();
}

void DevFooterBar::set_editor_frame_navigation(EditorFrameNavigation navigation) {
    editor_frame_navigation_ = std::move(navigation);
    editor_frame_navigation_.frame_count = std::max(0, editor_frame_navigation_.frame_count);
    if (editor_frame_navigation_.frame_count <= 0) {
        editor_frame_navigation_.selected_frame = 0;
        editor_frame_navigation_.visible = false;
    } else {
        editor_frame_navigation_.selected_frame = std::clamp(editor_frame_navigation_.selected_frame,
                                                             0,
                                                             editor_frame_navigation_.frame_count - 1);
    }

    if (!editor_prev_animation_button_) {
        editor_prev_animation_button_ =
            std::make_unique<DMButton>("Anim -", &DMStyles::SecondaryButton(), kEditorFrameNavButtonWidth, DMButton::height());
    }
    if (!editor_next_animation_button_) {
        editor_next_animation_button_ =
            std::make_unique<DMButton>("Anim +", &DMStyles::SecondaryButton(), kEditorFrameNavButtonWidth, DMButton::height());
    }
    if (!editor_prev_frame_button_) {
        editor_prev_frame_button_ =
            std::make_unique<DMButton>("Frame -", &DMStyles::SecondaryButton(), kEditorFrameNavButtonWidth, DMButton::height());
    }
    if (!editor_next_frame_button_) {
        editor_next_frame_button_ =
            std::make_unique<DMButton>("Frame +", &DMStyles::SecondaryButton(), kEditorFrameNavButtonWidth, DMButton::height());
    }

    if (editor_prev_animation_button_) {
        editor_prev_animation_button_->set_style(editor_nav_button_style(static_cast<bool>(editor_frame_navigation_.on_prev_animation)));
    }
    if (editor_next_animation_button_) {
        editor_next_animation_button_->set_style(editor_nav_button_style(static_cast<bool>(editor_frame_navigation_.on_next_animation)));
    }
    if (editor_prev_frame_button_) {
        editor_prev_frame_button_->set_style(editor_nav_button_style(static_cast<bool>(editor_frame_navigation_.on_prev_frame)));
    }
    if (editor_next_frame_button_) {
        editor_next_frame_button_->set_style(editor_nav_button_style(static_cast<bool>(editor_frame_navigation_.on_next_frame)));
    }

    editor_hovered_frame_index_ = -1;
    editor_pressed_frame_index_ = -1;
    ensure_editor_frame_visible(editor_frame_navigation_.selected_frame);
    layout_content();
}

void DevFooterBar::clear_editor_navigation() {
    editor_tabs_.clear();
    editor_frame_navigation_ = EditorFrameNavigation{};
    editor_tabs_row_rect_ = SDL_Rect{0, 0, 0, 0};
    editor_animation_label_rect_ = SDL_Rect{0, 0, 0, 0};
    editor_frame_strip_rect_ = SDL_Rect{0, 0, 0, 0};
    editor_frame_scroll_offset_ = 0.0f;
    editor_hovered_frame_index_ = -1;
    editor_pressed_frame_index_ = -1;
    layout_content();
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

    if (editor_navigation_enabled_) {
        used = handle_editor_navigation_event(e) || used;
    } else {
        if (settings_controls_visible_) {
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

    const bool draw_separator = !editor_navigation_enabled_ &&
        settings_controls_visible_ &&
        (grid_checkbox_ && grid_stepper_) &&
        (title_bounds_.w > 0 || !buttons_.empty());
    if (draw_separator) {
        SDL_Color separator = DMStyles::Border();
        separator.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(separator.a * 0.8f), 0, 255));
        SDL_SetRenderDrawColor(renderer, separator.r, separator.g, separator.b, separator.a);
        const int separator_x = std::min(rect_.x + rect_.w - 1, grid_controls_right_ + kFooterGroupGap / 2);
        SDL_RenderLine(renderer, separator_x, rect_.y + kFooterVerticalPadding, separator_x, rect_.y + rect_.h - kFooterVerticalPadding);
    }

    if (!button_group_dividers_.empty()) {
        SDL_Color divider = DMStyles::Border();
        divider.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(divider.a * 0.5f), 0, 255));
        SDL_SetRenderDrawColor(renderer, divider.r, divider.g, divider.b, divider.a);
        const int divider_top = rect_.y + kFooterVerticalPadding;
        const int divider_bottom = rect_.y + rect_.h - kFooterVerticalPadding;
        for (int x_pos : button_group_dividers_) {
            SDL_RenderLine(renderer, x_pos, divider_top, x_pos, divider_bottom);
        }
    }

    if (hide_button_) {
        hide_button_->render(renderer);
    }

    if (editor_navigation_enabled_) {
        render_editor_navigation(renderer);
        return;
    }

    if (settings_controls_visible_) {
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
    if (editor_navigation_enabled_) {
        layout_editor_navigation();
        return;
    }
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
    button_group_dividers_.clear();
    for (auto& btn : buttons_) {
        if (btn.widget) {
            btn.widget->set_rect(SDL_Rect{0, 0, 0, 0});
        }
    }

    int button_start = content_start_x();
    if (settings_controls_visible_ && grid_checkbox_ && grid_stepper_) {
        button_start = std::max(button_start, grid_controls_right_ + kFooterGroupGap);
    }
    if (title_bounds_.w > 0) {
        button_start = std::max(button_start, title_bounds_.x + title_bounds_.w + kFooterGroupGap);
    }

    const int right_limit = rect_.x + rect_.w - kFooterHorizontalPadding;
    const int available_width = right_limit - button_start;
    if (available_width <= 0) {
        return;
    }

    struct ButtonLayoutInfo {
        DMButton* widget = nullptr;
        int width = 0;
    };

    std::array<std::vector<ButtonLayoutInfo>, 4> grouped;
    for (auto& btn : buttons_) {
        if (!btn.widget) continue;
        const int width = std::max(btn.widget->preferred_width(), kFooterButtonMinWidth);
        const int group_index = std::clamp(static_cast<int>(btn.group),
                                           static_cast<int>(FooterButtonGroup::Primary),
                                           static_cast<int>(FooterButtonGroup::Utilities));
        grouped[static_cast<size_t>(group_index)].push_back({btn.widget.get(), width});
    }

    constexpr std::array<FooterButtonGroup, 4> display_order = {
        FooterButtonGroup::Primary,
        FooterButtonGroup::Panels,
        FooterButtonGroup::Actions,
        FooterButtonGroup::Utilities,
    };

    std::vector<FooterButtonGroup> active_groups;
    std::array<int, 4> group_total_widths{};
    for (auto group : display_order) {
        const auto& layout = grouped[static_cast<size_t>(group)];
        if (layout.empty()) {
            group_total_widths[static_cast<size_t>(group)] = 0;
            continue;
        }
        int width = 0;
        for (const auto& info : layout) {
            width += info.width;
        }
        width += kFooterButtonSpacing * std::max(0, static_cast<int>(layout.size()) - 1);
        group_total_widths[static_cast<size_t>(group)] = width;
        active_groups.push_back(group);
    }

    if (active_groups.empty()) {
        return;
    }

    const auto compute_total_width = [&](const std::vector<FooterButtonGroup>& groups) {
        int width = 0;
        for (auto group : groups) {
            width += group_total_widths[static_cast<size_t>(group)];
        }
        if (groups.size() > 1) {
            width += kFooterGroupGap * (static_cast<int>(groups.size()) - 1);
        }
        return width;
    };

    constexpr std::array<FooterButtonGroup, 4> removal_priority = {
        FooterButtonGroup::Utilities,
        FooterButtonGroup::Actions,
        FooterButtonGroup::Panels,
        FooterButtonGroup::Primary,
    };

    int total_width = compute_total_width(active_groups);
    while (!active_groups.empty() && total_width > available_width) {
        bool removed = false;
        for (auto group : removal_priority) {
            auto it = std::find(active_groups.begin(), active_groups.end(), group);
            if (it != active_groups.end()) {
                active_groups.erase(it);
                removed = true;
                break;
            }
        }
        if (!removed) {
            break;
        }
        total_width = compute_total_width(active_groups);
    }

    if (active_groups.empty()) {
        return;
    }

    const int y = rect_.y + (rect_.h - DMButton::height()) / 2;
    int current_right = right_limit;
    for (int group_index = static_cast<int>(active_groups.size()) - 1; group_index >= 0; --group_index) {
        const auto group = active_groups[group_index];
        const auto& layout = grouped[static_cast<size_t>(group)];
        if (layout.empty()) {
            continue;
        }
        const int group_width = group_total_widths[static_cast<size_t>(group)];
        const int group_start = current_right - group_width;
        int x = group_start;
        for (size_t i = 0; i < layout.size(); ++i) {
            const auto& info = layout[i];
            info.widget->set_rect(SDL_Rect{x, y, info.width, DMButton::height()});
            x += info.width;
            if (i + 1 < layout.size()) {
                x += kFooterButtonSpacing;
            }
        }
        if (group_index > 0 && kFooterGroupGap > 0) {
            const int divider_x = group_start - (kFooterGroupGap / 2);
            if (divider_x > rect_.x) {
                button_group_dividers_.push_back(divider_x);
            }
            current_right = group_start - kFooterGroupGap;
        } else {
            current_right = group_start;
        }
    }

    if (!button_group_dividers_.empty()) {
        std::sort(button_group_dividers_.begin(), button_group_dividers_.end());
    }
}

void DevFooterBar::layout_editor_navigation() {
    button_group_dividers_.clear();
    title_bounds_ = SDL_Rect{0, 0, 0, 0};
    editor_tabs_row_rect_ = SDL_Rect{0, 0, 0, 0};
    editor_animation_label_rect_ = SDL_Rect{0, 0, 0, 0};
    editor_frame_strip_rect_ = SDL_Rect{0, 0, 0, 0};

    if (movement_debug_checkbox_) movement_debug_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
    if (grid_checkbox_) grid_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
    if (grid_stepper_) grid_stepper_->set_rect(SDL_Rect{0, 0, 0, 0});
    for (auto& btn : buttons_) {
        if (btn.widget) {
            btn.widget->set_rect(SDL_Rect{0, 0, 0, 0});
        }
    }

    const int left = content_start_x();
    const int right = rect_.x + rect_.w - kFooterHorizontalPadding;
    const int available_width = std::max(0, right - left);
    if (available_width <= 0) {
        return;
    }

    int y = rect_.y + kFooterVerticalPadding;
    const int tab_height = DMButton::height();
    if (!editor_tabs_.empty()) {
        const int tab_count = static_cast<int>(editor_tabs_.size());
        const int min_tab_width = 32;
        const int max_tab_gap = kEditorTabGap;
        const int tab_gap = (tab_count > 1)
            ? std::clamp((available_width - (tab_count * min_tab_width)) / (tab_count - 1), 2, max_tab_gap)
            : 0;
        const int total_gap = std::max(0, tab_count - 1) * tab_gap;
        const int tab_width = std::max(min_tab_width, (available_width - total_gap) / std::max(1, tab_count));
        int x = left;
        for (auto& tab : editor_tabs_) {
            if (!tab.widget) {
                tab.widget = std::make_unique<DMButton>(tab.label, editor_tab_style_for(tab), tab_width, tab_height);
            }
            tab.widget->set_style(editor_tab_style_for(tab));
            tab.widget->set_rect(SDL_Rect{x, y, tab_width, tab_height});
            x += tab_width + tab_gap;
        }
        editor_tabs_row_rect_ = SDL_Rect{left, y, available_width, tab_height};
        y += tab_height + kEditorFrameRowGap;
    } else {
        for (auto& tab : editor_tabs_) {
            if (tab.widget) {
                tab.widget->set_rect(SDL_Rect{0, 0, 0, 0});
            }
        }
    }

    if (!editor_frame_navigation_.visible) {
        if (editor_prev_animation_button_) editor_prev_animation_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (editor_next_animation_button_) editor_next_animation_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (editor_prev_frame_button_) editor_prev_frame_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (editor_next_frame_button_) editor_next_frame_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        return;
    }

    editor_animation_label_rect_ = SDL_Rect{left, y, available_width, kEditorAnimationLabelHeight};
    y += kEditorAnimationLabelHeight + kEditorFrameRowGap;

    if (!editor_prev_animation_button_) {
        editor_prev_animation_button_ =
            std::make_unique<DMButton>("Anim -", &DMStyles::SecondaryButton(), kEditorFrameNavButtonWidth, DMButton::height());
    }
    if (!editor_next_animation_button_) {
        editor_next_animation_button_ =
            std::make_unique<DMButton>("Anim +", &DMStyles::SecondaryButton(), kEditorFrameNavButtonWidth, DMButton::height());
    }
    if (!editor_prev_frame_button_) {
        editor_prev_frame_button_ =
            std::make_unique<DMButton>("Frame -", &DMStyles::SecondaryButton(), kEditorFrameNavButtonWidth, DMButton::height());
    }
    if (!editor_next_frame_button_) {
        editor_next_frame_button_ =
            std::make_unique<DMButton>("Frame +", &DMStyles::SecondaryButton(), kEditorFrameNavButtonWidth, DMButton::height());
    }

    if (editor_prev_animation_button_) {
        editor_prev_animation_button_->set_style(
            editor_nav_button_style(static_cast<bool>(editor_frame_navigation_.on_prev_animation)));
    }
    if (editor_next_animation_button_) {
        editor_next_animation_button_->set_style(
            editor_nav_button_style(static_cast<bool>(editor_frame_navigation_.on_next_animation)));
    }
    if (editor_prev_frame_button_) {
        editor_prev_frame_button_->set_style(
            editor_nav_button_style(static_cast<bool>(editor_frame_navigation_.on_prev_frame)));
    }
    if (editor_next_frame_button_) {
        editor_next_frame_button_->set_style(
            editor_nav_button_style(static_cast<bool>(editor_frame_navigation_.on_next_frame)));
    }

    const int button_h = DMButton::height();
    const int button_y = y + (kEditorFrameStripHeight - button_h) / 2;
    const int button_gap = 6;
    const int min_strip_w = 120;
    const int desired_nav_button_w = kEditorFrameNavButtonWidth;
    const int reserved_for_buttons = std::max(0, available_width - min_strip_w - 20);
    const int cluster_budget = reserved_for_buttons / 2;
    const int nav_button_w = std::clamp((cluster_budget - button_gap) / 2, 34, desired_nav_button_w);
    const int left_cluster_w = (nav_button_w * 2) + button_gap;
    const int right_cluster_w = left_cluster_w;

    const SDL_Rect anim_prev_rect{left, button_y, nav_button_w, button_h};
    const SDL_Rect anim_next_rect{left + nav_button_w + button_gap, button_y, nav_button_w, button_h};
    const SDL_Rect frame_prev_rect{right - right_cluster_w, button_y, nav_button_w, button_h};
    const SDL_Rect frame_next_rect{
        right - nav_button_w,
        button_y,
        nav_button_w,
        button_h};

    editor_prev_animation_button_->set_rect(anim_prev_rect);
    editor_next_animation_button_->set_rect(anim_next_rect);
    editor_prev_frame_button_->set_rect(frame_prev_rect);
    editor_next_frame_button_->set_rect(frame_next_rect);

    const int strip_x = anim_next_rect.x + anim_next_rect.w + 10;
    const int strip_right = frame_prev_rect.x - 10;
    editor_frame_strip_rect_ = SDL_Rect{
        strip_x,
        y,
        std::max(0, strip_right - strip_x),
        kEditorFrameStripHeight};

    clamp_editor_frame_scroll();
    ensure_editor_frame_visible(editor_frame_navigation_.selected_frame);
}

void DevFooterBar::clamp_editor_frame_scroll() {
    if (editor_frame_navigation_.frame_count <= 0 || editor_frame_strip_rect_.w <= 0) {
        editor_frame_scroll_offset_ = 0.0f;
        return;
    }
    const float stride = static_cast<float>(kEditorFrameChipWidth + kEditorFrameChipGap);
    const float total_width =
        static_cast<float>(editor_frame_navigation_.frame_count) * stride - static_cast<float>(kEditorFrameChipGap);
    const float max_scroll = std::max(0.0f, total_width - static_cast<float>(editor_frame_strip_rect_.w));
    editor_frame_scroll_offset_ = std::clamp(editor_frame_scroll_offset_, 0.0f, max_scroll);
}

void DevFooterBar::ensure_editor_frame_visible(int frame_index) {
    if (editor_frame_navigation_.frame_count <= 0 || editor_frame_strip_rect_.w <= 0) {
        editor_frame_scroll_offset_ = 0.0f;
        return;
    }
    const int clamped_index = std::clamp(frame_index, 0, editor_frame_navigation_.frame_count - 1);
    const float stride = static_cast<float>(kEditorFrameChipWidth + kEditorFrameChipGap);
    const float frame_start = static_cast<float>(clamped_index) * stride;
    const float frame_end = frame_start + static_cast<float>(kEditorFrameChipWidth);
    const float view_start = editor_frame_scroll_offset_;
    const float view_end = view_start + static_cast<float>(editor_frame_strip_rect_.w);
    if (frame_start < view_start) {
        editor_frame_scroll_offset_ = std::max(0.0f, frame_start - static_cast<float>(kEditorFrameChipGap));
    } else if (frame_end > view_end) {
        editor_frame_scroll_offset_ = frame_end - static_cast<float>(editor_frame_strip_rect_.w) +
            static_cast<float>(kEditorFrameChipGap);
    }
    clamp_editor_frame_scroll();
}

int DevFooterBar::editor_frame_index_at_point(const SDL_Point& point) const {
    if (editor_frame_navigation_.frame_count <= 0 || editor_frame_strip_rect_.w <= 0 ||
        !SDL_PointInRect(&point, &editor_frame_strip_rect_)) {
        return -1;
    }
    const float stride = static_cast<float>(kEditorFrameChipWidth + kEditorFrameChipGap);
    const float relative_x = static_cast<float>(point.x - editor_frame_strip_rect_.x) + editor_frame_scroll_offset_;
    if (relative_x < 0.0f) {
        return -1;
    }
    const int index = static_cast<int>(relative_x / stride);
    if (index < 0 || index >= editor_frame_navigation_.frame_count) {
        return -1;
    }
    const float tile_offset = relative_x - (static_cast<float>(index) * stride);
    if (tile_offset > static_cast<float>(kEditorFrameChipWidth)) {
        return -1;
    }
    return index;
}

SDL_Rect DevFooterBar::editor_frame_chip_rect(int frame_index) const {
    const float stride = static_cast<float>(kEditorFrameChipWidth + kEditorFrameChipGap);
    const float x = static_cast<float>(editor_frame_strip_rect_.x) +
        static_cast<float>(frame_index) * stride - editor_frame_scroll_offset_;
    return SDL_Rect{
        static_cast<int>(std::lround(x)),
        editor_frame_strip_rect_.y + (editor_frame_strip_rect_.h - (kEditorFrameStripHeight - 6)) / 2,
        kEditorFrameChipWidth,
        kEditorFrameStripHeight - 6};
}

bool DevFooterBar::handle_editor_navigation_event(const SDL_Event& e) {
    bool used = false;

    auto trigger_button = [&](DMButton* button, const std::function<void()>& callback) {
        if (!button) {
            return false;
        }
        if (!button->handle_event(e)) {
            return false;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            e.button.button == SDL_BUTTON_LEFT &&
            callback) {
            callback();
        }
        return true;
    };

    for (auto& tab : editor_tabs_) {
        if (!tab.widget) {
            continue;
        }
        if (tab.widget->handle_event(e)) {
            used = true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                e.button.button == SDL_BUTTON_LEFT &&
                tab.enabled &&
                tab.on_select) {
                tab.on_select();
            }
        }
    }

    used = trigger_button(editor_prev_animation_button_.get(), editor_frame_navigation_.on_prev_animation) || used;
    used = trigger_button(editor_next_animation_button_.get(), editor_frame_navigation_.on_next_animation) || used;
    used = trigger_button(editor_prev_frame_button_.get(), editor_frame_navigation_.on_prev_frame) || used;
    used = trigger_button(editor_next_frame_button_.get(), editor_frame_navigation_.on_next_frame) || used;

    if (!editor_frame_navigation_.visible || editor_frame_navigation_.frame_count <= 0) {
        return used;
    }

    SDL_Point pointer{0, 0};
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        pointer = SDL_Point{
            static_cast<int>(std::lround(e.motion.x)),
            static_cast<int>(std::lround(e.motion.y))
        };
        editor_hovered_frame_index_ = editor_frame_index_at_point(pointer);
        const bool over_label = SDL_PointInRect(&pointer, &editor_animation_label_rect_);
        used = used || over_label || editor_hovered_frame_index_ >= 0;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        pointer = SDL_Point{
            static_cast<int>(std::lround(e.button.x)),
            static_cast<int>(std::lround(e.button.y))
        };
        editor_pressed_frame_index_ = editor_frame_index_at_point(pointer);
        editor_hovered_frame_index_ = editor_pressed_frame_index_;
        if (editor_pressed_frame_index_ >= 0) {
            used = true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        pointer = SDL_Point{
            static_cast<int>(std::lround(e.button.x)),
            static_cast<int>(std::lround(e.button.y))
        };
        const int released_index = editor_frame_index_at_point(pointer);
        if (released_index >= 0 && released_index == editor_pressed_frame_index_) {
            editor_frame_navigation_.selected_frame = released_index;
            ensure_editor_frame_visible(released_index);
            if (editor_frame_navigation_.on_select_frame) {
                editor_frame_navigation_.on_select_frame(released_index);
            }
            used = true;
        } else if (editor_frame_navigation_.animation_clickable &&
                   SDL_PointInRect(&pointer, &editor_animation_label_rect_)) {
            if (editor_frame_navigation_.on_activate_animation) {
                editor_frame_navigation_.on_activate_animation();
            }
            used = true;
        }
        editor_pressed_frame_index_ = -1;
        editor_hovered_frame_index_ = released_index;
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        sdl_mouse_util::GetMouseState(&pointer.x, &pointer.y);
        if (SDL_PointInRect(&pointer, &editor_frame_strip_rect_)) {
            int delta_x = e.wheel.x;
            int delta_y = e.wheel.y;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                delta_x = -delta_x;
                delta_y = -delta_y;
            }
            const int steps = (delta_x != 0) ? delta_x : delta_y;
            const float stride = static_cast<float>(kEditorFrameChipWidth + kEditorFrameChipGap);
            editor_frame_scroll_offset_ = editor_frame_scroll_offset_ - static_cast<float>(steps) * stride;
            clamp_editor_frame_scroll();
            used = true;
        }
    }

    return used;
}

void DevFooterBar::render_editor_navigation(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }

    for (const auto& tab : editor_tabs_) {
        if (!tab.widget) {
            continue;
        }
        tab.widget->render(renderer);
    }

    if (!editor_frame_navigation_.visible) {
        return;
    }

    DMLabelStyle animation_style = DMStyles::Label();
    animation_style.font_size = 14;
    animation_style.color = editor_frame_navigation_.animation_clickable
        ? DMStyles::ButtonFocusOutline()
        : DMStyles::Label().color;
    const SDL_Point animation_text_size =
        DMFontCache::instance().measure_text(animation_style, editor_frame_navigation_.animation_label);
    const int animation_text_x = editor_animation_label_rect_.x +
        std::max(0, (editor_animation_label_rect_.w - animation_text_size.x) / 2);
    const int animation_text_y = editor_animation_label_rect_.y +
        std::max(0, (editor_animation_label_rect_.h - animation_text_size.y) / 2);
    DMFontCache::instance().draw_text(renderer,
                                      animation_style,
                                      editor_frame_navigation_.animation_label,
                                      animation_text_x,
                                      animation_text_y);

    if (editor_frame_navigation_.animation_clickable &&
        editor_animation_label_rect_.w > 0 &&
        editor_animation_label_rect_.h > 0) {
        SDL_Color underline = DMStyles::ButtonFocusOutline();
        underline.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(underline.a * 0.85f), 0, 255));
        SDL_SetRenderDrawColor(renderer, underline.r, underline.g, underline.b, underline.a);
        const int y = editor_animation_label_rect_.y + editor_animation_label_rect_.h - 2;
        SDL_RenderLine(renderer,
                       editor_animation_label_rect_.x + 4,
                       y,
                       editor_animation_label_rect_.x + editor_animation_label_rect_.w - 4,
                       y);
    }

    if (editor_prev_animation_button_) editor_prev_animation_button_->render(renderer);
    if (editor_next_animation_button_) editor_next_animation_button_->render(renderer);
    if (editor_prev_frame_button_) editor_prev_frame_button_->render(renderer);
    if (editor_next_frame_button_) editor_next_frame_button_->render(renderer);

    if (editor_frame_strip_rect_.w <= 0 || editor_frame_strip_rect_.h <= 0 || editor_frame_navigation_.frame_count <= 0) {
        return;
    }

    SDL_Color strip_bg = dm_draw::DarkenColor(DMStyles::PanelBG(), 0.08f);
    dm_draw::DrawRoundedSolidRect(renderer, editor_frame_strip_rect_, 8, strip_bg);
    dm_draw::DrawRoundedOutline(renderer, editor_frame_strip_rect_, 8, 1, DMStyles::Border());

    SDL_Rect previous_clip{};
    SDL_GetRenderClipRect(renderer, &previous_clip);
    const bool clipping_enabled = SDL_RenderClipEnabled(renderer);
    SDL_SetRenderClipRect(renderer, &editor_frame_strip_rect_);

    for (int i = 0; i < editor_frame_navigation_.frame_count; ++i) {
        SDL_Rect chip = editor_frame_chip_rect(i);
        if (chip.x + chip.w < editor_frame_strip_rect_.x ||
            chip.x > editor_frame_strip_rect_.x + editor_frame_strip_rect_.w) {
            continue;
        }

        const bool selected = i == editor_frame_navigation_.selected_frame;
        const bool hovered = i == editor_hovered_frame_index_;
        SDL_Color chip_bg = selected ? kEditorFrameSelectedColor : dm_draw::DarkenColor(DMStyles::PanelHeader(), 0.04f);
        dm_draw::DrawRoundedSolidRect(renderer, chip, 7, chip_bg);
        const SDL_Color border = hovered ? DMStyles::HighlightColor() : DMStyles::Border();
        dm_draw::DrawRoundedOutline(renderer, chip, 7, 1, border);

        const int preview_pad = 3;
        SDL_Rect preview_rect{
            chip.x + preview_pad,
            chip.y + preview_pad,
            std::max(0, chip.w - preview_pad * 2),
            std::max(0, chip.h - preview_pad * 2)
        };
        if (editor_frame_navigation_.frame_texture_provider) {
            if (SDL_Texture* frame_texture = editor_frame_navigation_.frame_texture_provider(i)) {
                float tex_w = 0.0f;
                float tex_h = 0.0f;
                if (SDL_GetTextureSize(frame_texture, &tex_w, &tex_h) &&
                    tex_w > 1.0f && tex_h > 1.0f &&
                    preview_rect.w > 1 && preview_rect.h > 1) {
                    const float scale = std::min(
                        static_cast<float>(preview_rect.w) / tex_w,
                        static_cast<float>(preview_rect.h) / tex_h);
                    const int draw_w = std::max(1, static_cast<int>(std::lround(tex_w * scale)));
                    const int draw_h = std::max(1, static_cast<int>(std::lround(tex_h * scale)));
                    SDL_FRect dst{
                        static_cast<float>(preview_rect.x + (preview_rect.w - draw_w) / 2),
                        static_cast<float>(preview_rect.y + (preview_rect.h - draw_h) / 2),
                        static_cast<float>(draw_w),
                        static_cast<float>(draw_h)
                    };
                    sdl_render::Texture(renderer, frame_texture, nullptr, &dst);
                }
            }
        }

        const SDL_Rect label_bg{
            chip.x + 2,
            chip.y + 2,
            std::max(14, chip.w / 3),
            14
        };
        SDL_Color label_bg_color = dm_draw::DarkenColor(chip_bg, 0.28f);
        label_bg_color.a = 210;
        dm_draw::DrawRoundedSolidRect(renderer, label_bg, 5, label_bg_color);
        DMLabelStyle frame_style = DMStyles::Label();
        frame_style.font_size = 11;
        frame_style.color = selected ? SDL_Color{255, 255, 255, 255} : DMStyles::Label().color;
        const std::string label = std::to_string(i + 1);
        const SDL_Point size = DMFontCache::instance().measure_text(frame_style, label);
        const int text_x = label_bg.x + std::max(0, (label_bg.w - size.x) / 2);
        const int text_y = label_bg.y + std::max(0, (label_bg.h - size.y) / 2);
        DMFontCache::instance().draw_text(renderer, frame_style, label, text_x, text_y);
    }

    if (clipping_enabled) {
        SDL_SetRenderClipRect(renderer, &previous_clip);
    } else {
        SDL_SetRenderClipRect(renderer, nullptr);
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

void DevFooterBar::set_grid_overlay_enabled(bool enabled, bool notify_callback) {
    if (grid_overlay_enabled_ != enabled) {
        grid_overlay_enabled_ = enabled;
        if (grid_checkbox_) {
            grid_checkbox_->set_value(enabled);
        }
        if (notify_callback && on_grid_overlay_toggle_) {
            on_grid_overlay_toggle_(enabled);
        }
    }
}

void DevFooterBar::set_grid_resolution(int resolution, bool notify_callback) {
    if (grid_resolution_ != resolution) {
        grid_resolution_ = resolution;
        if (grid_stepper_) {
            grid_stepper_->set_value(resolution);
        }
        if (notify_callback && on_grid_resolution_change_) {
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
        if (movement_debug_checkbox_) movement_debug_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (grid_checkbox_) grid_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (grid_stepper_) grid_stepper_->set_rect(SDL_Rect{0, 0, 0, 0});
        return;
    }
    if (!movement_debug_checkbox_ || !grid_checkbox_ || !grid_stepper_) {
        return;
    }

    int x = content_start_x();
    const int checkbox_y = rect_.y + (rect_.h - DMCheckbox::height()) / 2;
    const int stepper_y = rect_.y + (rect_.h - DMNumericStepper::height()) / 2;
    const int gap = DMSpacing::small_gap();

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




