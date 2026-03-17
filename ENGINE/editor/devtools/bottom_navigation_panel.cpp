#include "bottom_navigation_panel.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "dm_styles.hpp"
#include "draw_utils.hpp"
#include "font_cache.hpp"
#include "widgets.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"

namespace {

constexpr int kBottomMargin = 18;
constexpr int kPanelPadding = 12;
constexpr int kPanelGap = 12;
constexpr int kPanelHeight = 88;
constexpr int kCompactPanelHeight = 58;
constexpr int kActionButtonMinWidth = 150;
constexpr int kAxisWidth = 226;
constexpr int kAxisButtonWidth = 68;
constexpr int kAxisButtonGap = 8;
constexpr int kAxisCardCornerRadius = 8;
constexpr int kTitleFontSize = 12;
constexpr int kValueFontSize = 15;

std::string fit_text(const DMLabelStyle& style, const std::string& text, int max_width) {
    if (max_width <= 0 || text.empty()) {
        return {};
    }
    if (DMFontCache::instance().measure_text(style, text).x <= max_width) {
        return text;
    }

    static const std::string kEllipsis = "...";
    std::string trimmed = text;
    while (!trimmed.empty()) {
        trimmed.pop_back();
        const std::string candidate = trimmed + kEllipsis;
        if (DMFontCache::instance().measure_text(style, candidate).x <= max_width) {
            return candidate;
        }
    }
    return kEllipsis;
}

void draw_centered_text(SDL_Renderer* renderer,
                        const DMLabelStyle& style,
                        const std::string& text,
                        const SDL_Rect& rect) {
    if (!renderer || rect.w <= 0 || rect.h <= 0 || text.empty()) {
        return;
    }

    const std::string fitted = fit_text(style, text, rect.w);
    if (fitted.empty()) {
        return;
    }

    const SDL_Point size = DMFontCache::instance().measure_text(style, fitted);
    const int draw_x = rect.x + std::max(0, (rect.w - size.x) / 2);
    const int draw_y = rect.y + std::max(0, (rect.h - size.y) / 2);
    DMFontCache::instance().draw_text(renderer, style, fitted, draw_x, draw_y);
}

}  // namespace

BottomNavigationPanel::BottomNavigationPanel() = default;

BottomNavigationPanel::~BottomNavigationPanel() = default;

void BottomNavigationPanel::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    layout_dirty_ = true;
}

void BottomNavigationPanel::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void BottomNavigationPanel::set_action(const std::string& label,
                                       std::function<void()> callback,
                                       bool emphasized) {
    ensure_widgets();
    action_visible_ = !label.empty();
    if (action_label_ != label) {
        action_label_ = label;
        if (action_button_) {
            action_button_->set_text(action_label_);
        }
        layout_dirty_ = true;
    }
    on_action_ = std::move(callback);
    if (action_emphasized_ != emphasized) {
        action_emphasized_ = emphasized;
        set_action_style();
    }
}

void BottomNavigationPanel::clear_action() {
    action_label_.clear();
    action_visible_ = false;
    on_action_ = {};
    layout_dirty_ = true;
}

void BottomNavigationPanel::set_primary_navigation(const std::string& label,
                                                   const std::string& value,
                                                   std::function<void()> on_prev,
                                                   std::function<void()> on_next,
                                                   bool visible) {
    primary_axis_.label = label;
    primary_axis_.value = value;
    primary_axis_.on_prev = std::move(on_prev);
    primary_axis_.on_next = std::move(on_next);
    if (primary_axis_.visible != visible) {
        primary_axis_.visible = visible;
        layout_dirty_ = true;
    }
}

void BottomNavigationPanel::set_secondary_navigation(const std::string& label,
                                                     const std::string& value,
                                                     std::function<void()> on_prev,
                                                     std::function<void()> on_next,
                                                     bool visible) {
    secondary_axis_.label = label;
    secondary_axis_.value = value;
    secondary_axis_.on_prev = std::move(on_prev);
    secondary_axis_.on_next = std::move(on_next);
    if (secondary_axis_.visible != visible) {
        secondary_axis_.visible = visible;
        layout_dirty_ = true;
    }
}

void BottomNavigationPanel::clear_navigation() {
    if (!primary_axis_.visible && !secondary_axis_.visible) {
        primary_axis_.label.clear();
        primary_axis_.value.clear();
        secondary_axis_.label.clear();
        secondary_axis_.value.clear();
        return;
    }
    primary_axis_ = {};
    secondary_axis_ = {};
    layout_dirty_ = true;
}

bool BottomNavigationPanel::handle_event(const SDL_Event& event) {
    if (!visible_) {
        return false;
    }

    ensure_widgets();
    update_layout();

    auto handle_button = [&](DMButton* button, const std::function<void()>& on_click) {
        if (!button) {
            return false;
        }
        if (!button->handle_event(event)) {
            return false;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_click) {
            on_click();
        }
        return true;
    };

    bool handled = false;
    if (action_visible_) {
        handled = handle_button(action_button_.get(), on_action_) || handled;
    }
    if (primary_axis_.visible) {
        handled = handle_button(primary_prev_button_.get(), primary_axis_.on_prev) || handled;
        handled = handle_button(primary_next_button_.get(), primary_axis_.on_next) || handled;
    }
    if (secondary_axis_.visible) {
        handled = handle_button(secondary_prev_button_.get(), secondary_axis_.on_prev) || handled;
        handled = handle_button(secondary_next_button_.get(), secondary_axis_.on_next) || handled;
    }
    if (handled) {
        return true;
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
        return true;
    }
    return false;
}

void BottomNavigationPanel::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) {
        return;
    }

    const_cast<BottomNavigationPanel*>(this)->ensure_widgets();
    const_cast<BottomNavigationPanel*>(this)->update_layout();

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_Color panel_top = DMStyles::PanelHeader();
    const SDL_Color panel_bottom = dm_draw::DarkenColor(panel_top, 0.24f);
    dm_draw::DrawRoundedGradientRect(renderer, panel_rect_, DMStyles::CornerRadius() + 2, panel_top, panel_bottom);
    dm_draw::DrawRoundedOutline(renderer, panel_rect_, DMStyles::CornerRadius() + 2, 1, DMStyles::Border());

    SDL_Color glow = DMStyles::HighlightColor();
    glow.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(glow.a * 0.24f), 0, 255));
    SDL_SetRenderDrawColor(renderer, glow.r, glow.g, glow.b, glow.a);
    SDL_RenderLine(renderer,
                   panel_rect_.x + 1,
                   panel_rect_.y + 1,
                   panel_rect_.x + panel_rect_.w - 2,
                   panel_rect_.y + 1);

    auto render_axis = [&](const NavigationAxis& axis, const SDL_Rect& value_rect) {
        if (!axis.visible || value_rect.w <= 0 || value_rect.h <= 0) {
            return;
        }

        const SDL_Color card_fill = dm_draw::DarkenColor(DMStyles::PanelBG(), 0.08f);
        dm_draw::DrawBeveledRect(renderer,
                                 value_rect,
                                 kAxisCardCornerRadius,
                                 1,
                                 card_fill,
                                 DMStyles::HighlightColor(),
                                 DMStyles::ShadowColor(),
                                 false,
                                 DMStyles::HighlightIntensity() * 0.85f,
                                 DMStyles::ShadowIntensity() * 0.9f);
        dm_draw::DrawRoundedOutline(renderer, value_rect, kAxisCardCornerRadius, 1, DMStyles::Border());

        DMLabelStyle title_style = DMStyles::Label();
        title_style.font_size = kTitleFontSize;
        title_style.color = dm_draw::LightenColor(title_style.color, 0.18f);

        DMLabelStyle value_style = DMStyles::Label();
        value_style.font_size = kValueFontSize;
        value_style.color = DMStyles::ButtonFocusOutline();

        SDL_Rect title_rect{value_rect.x + 6, value_rect.y + 6, value_rect.w - 12, 14};
        SDL_Rect text_rect{value_rect.x + 6, value_rect.y + 24, value_rect.w - 12, value_rect.h - 30};
        draw_centered_text(renderer, title_style, axis.label, title_rect);
        draw_centered_text(renderer, value_style, axis.value, text_rect);
    };

    if (action_visible_ && action_button_) {
        action_button_->render(renderer);
    }
    if (primary_axis_.visible) {
        render_axis(primary_axis_, primary_axis_.value_rect);
        if (primary_prev_button_) primary_prev_button_->render(renderer);
        if (primary_next_button_) primary_next_button_->render(renderer);
    }
    if (secondary_axis_.visible) {
        render_axis(secondary_axis_, secondary_axis_.value_rect);
        if (secondary_prev_button_) secondary_prev_button_->render(renderer);
        if (secondary_next_button_) secondary_next_button_->render(renderer);
    }
}

bool BottomNavigationPanel::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    const_cast<BottomNavigationPanel*>(this)->update_layout();
    return point_in_rect(x, y, panel_rect_);
}

const SDL_Rect& BottomNavigationPanel::rect() const {
    const_cast<BottomNavigationPanel*>(this)->update_layout();
    return panel_rect_;
}

void BottomNavigationPanel::ensure_widgets() {
    if (!action_button_) {
        action_button_ = std::make_unique<DMButton>("Action", &DMStyles::PrimaryButton(), kActionButtonMinWidth, DMButton::height());
    }
    if (!primary_prev_button_) {
        primary_prev_button_ = std::make_unique<DMButton>("Prev", &DMStyles::SecondaryButton(), kAxisButtonWidth, DMButton::height());
    }
    if (!primary_next_button_) {
        primary_next_button_ = std::make_unique<DMButton>("Next", &DMStyles::SecondaryButton(), kAxisButtonWidth, DMButton::height());
    }
    if (!secondary_prev_button_) {
        secondary_prev_button_ = std::make_unique<DMButton>("Prev", &DMStyles::SecondaryButton(), kAxisButtonWidth, DMButton::height());
    }
    if (!secondary_next_button_) {
        secondary_next_button_ = std::make_unique<DMButton>("Next", &DMStyles::SecondaryButton(), kAxisButtonWidth, DMButton::height());
    }
    set_action_style();
}

void BottomNavigationPanel::update_layout() const {
    if (!layout_dirty_) {
        return;
    }

    const_cast<BottomNavigationPanel*>(this)->ensure_widgets();

    const bool show_primary = primary_axis_.visible;
    const bool show_secondary = secondary_axis_.visible;
    const int visible_axes = (show_primary ? 1 : 0) + (show_secondary ? 1 : 0);
    const int panel_height = visible_axes > 0 ? kPanelHeight : kCompactPanelHeight;

    int action_width = 0;
    if (action_visible_ && action_button_) {
        action_width = kActionButtonMinWidth;
        action_width = std::max(kActionButtonMinWidth, action_button_->preferred_width());
    }
    const int nav_width = visible_axes > 0
        ? (visible_axes * kAxisWidth) + (std::max(0, visible_axes - 1) * kPanelGap)
        : 0;
    const int content_width = (action_visible_ ? action_width : 0) +
        ((action_visible_ && nav_width > 0) ? kPanelGap : 0) + nav_width;
    const int panel_width = std::min(std::max(content_width + (kPanelPadding * 2),
                                              (action_visible_ ? action_width : nav_width) + (kPanelPadding * 2)),
                                     std::max(0, screen_w_ - (kBottomMargin * 2)));

    panel_rect_.w = panel_width;
    panel_rect_.h = panel_height;
    panel_rect_.x = std::max(0, (screen_w_ - panel_rect_.w) / 2);
    panel_rect_.y = std::max(0, screen_h_ - panel_rect_.h - kBottomMargin);

    const int action_x = panel_rect_.x + kPanelPadding;
    const int action_y = panel_rect_.y + (panel_rect_.h - DMButton::height()) / 2;
    if (visible_axes == 0) {
        const int centered_x = panel_rect_.x + (panel_rect_.w - action_width) / 2;
        if (action_visible_ && action_button_) {
            action_button_->set_rect(SDL_Rect{centered_x, action_y, action_width, DMButton::height()});
        } else if (action_button_) {
            action_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        primary_axis_.bounds = SDL_Rect{0, 0, 0, 0};
        primary_axis_.value_rect = SDL_Rect{0, 0, 0, 0};
        secondary_axis_.bounds = SDL_Rect{0, 0, 0, 0};
        secondary_axis_.value_rect = SDL_Rect{0, 0, 0, 0};
        layout_dirty_ = false;
        return;
    }

    if (action_visible_ && action_button_) {
        action_button_->set_rect(SDL_Rect{action_x, action_y, action_width, DMButton::height()});
    } else if (action_button_) {
        action_button_->set_rect(SDL_Rect{0, 0, 0, 0});
    }

    int axis_x = action_x + (action_visible_ ? (action_width + kPanelGap) : 0);
    const int axis_y = panel_rect_.y + (panel_rect_.h - 54) / 2;
    const auto layout_axis = [&](NavigationAxis& axis, DMButton* prev_button, DMButton* next_button) {
        if (!axis.visible) {
            axis.bounds = SDL_Rect{0, 0, 0, 0};
            axis.value_rect = SDL_Rect{0, 0, 0, 0};
            return;
        }
        axis.bounds = SDL_Rect{axis_x, axis_y, kAxisWidth, 54};
        const int button_y = axis_y + (54 - DMButton::height()) / 2;
        const SDL_Rect prev_rect{axis_x, button_y, kAxisButtonWidth, DMButton::height()};
        const SDL_Rect next_rect{axis_x + kAxisWidth - kAxisButtonWidth, button_y, kAxisButtonWidth, DMButton::height()};
        const SDL_Rect value_rect{
            prev_rect.x + prev_rect.w + kAxisButtonGap,
            axis_y,
            kAxisWidth - (2 * kAxisButtonWidth) - (2 * kAxisButtonGap),
            54};
        axis.value_rect = value_rect;
        if (prev_button) {
            prev_button->set_rect(prev_rect);
        }
        if (next_button) {
            next_button->set_rect(next_rect);
        }
        axis_x += kAxisWidth + kPanelGap;
    };

    layout_axis(const_cast<NavigationAxis&>(primary_axis_), primary_prev_button_.get(), primary_next_button_.get());
    layout_axis(const_cast<NavigationAxis&>(secondary_axis_), secondary_prev_button_.get(), secondary_next_button_.get());

    layout_dirty_ = false;
}

void BottomNavigationPanel::set_action_style() {
    if (!action_button_) {
        return;
    }
    action_button_->set_style(action_emphasized_ ? &DMStyles::AccentButton() : &DMStyles::PrimaryButton());
}

bool BottomNavigationPanel::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point point{x, y};
    return SDL_PointInRect(&point, &rect);
}
