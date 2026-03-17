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
constexpr int kPanelGap = 8;
constexpr int kPanelCornerRadius = 8;
constexpr int kDpadButtonWidth = 72;
constexpr int kDpadCenterMinWidth = 164;
constexpr int kDpadCenterMaxWidth = 300;
constexpr int kDpadCenterHeight = 46;
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

void BottomNavigationPanel::set_dpad_navigation(const std::string& center_value,
                                                std::function<void()> on_up,
                                                std::function<void()> on_down,
                                                std::function<void()> on_left,
                                                std::function<void()> on_right,
                                                bool visible) {
    dpad_.center_value = center_value;
    dpad_.on_up = std::move(on_up);
    dpad_.on_down = std::move(on_down);
    dpad_.on_left = std::move(on_left);
    dpad_.on_right = std::move(on_right);
    if (dpad_.visible != visible) {
        dpad_.visible = visible;
        layout_dirty_ = true;
    } else {
        layout_dirty_ = true;
    }
}

void BottomNavigationPanel::clear_navigation() {
    if (!dpad_.visible && dpad_.center_value.empty()) {
        return;
    }
    dpad_ = {};
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
    if (dpad_.visible) {
        handled = handle_button(dpad_up_button_.get(), dpad_.on_up) || handled;
        handled = handle_button(dpad_down_button_.get(), dpad_.on_down) || handled;
        handled = handle_button(dpad_left_button_.get(), dpad_.on_left) || handled;
        handled = handle_button(dpad_right_button_.get(), dpad_.on_right) || handled;
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

    if (dpad_.visible && dpad_.center_rect.w > 0 && dpad_.center_rect.h > 0) {
        const SDL_Color card_fill = dm_draw::DarkenColor(DMStyles::PanelBG(), 0.08f);
        dm_draw::DrawBeveledRect(renderer,
                                 dpad_.center_rect,
                                 kPanelCornerRadius,
                                 1,
                                 card_fill,
                                 DMStyles::HighlightColor(),
                                 DMStyles::ShadowColor(),
                                 false,
                                 DMStyles::HighlightIntensity() * 0.85f,
                                 DMStyles::ShadowIntensity() * 0.9f);
        dm_draw::DrawRoundedOutline(renderer, dpad_.center_rect, kPanelCornerRadius, 1, DMStyles::Border());

        DMLabelStyle title_style = DMStyles::Label();
        title_style.font_size = kTitleFontSize;
        title_style.color = dm_draw::LightenColor(title_style.color, 0.18f);

        DMLabelStyle value_style = DMStyles::Label();
        value_style.font_size = kValueFontSize;
        value_style.color = DMStyles::ButtonFocusOutline();

        SDL_Rect title_rect{dpad_.center_rect.x + 6, dpad_.center_rect.y + 5, dpad_.center_rect.w - 12, 14};
        SDL_Rect text_rect{dpad_.center_rect.x + 6, dpad_.center_rect.y + 21, dpad_.center_rect.w - 12, dpad_.center_rect.h - 25};
        draw_centered_text(renderer, title_style, "Animation / Frame", title_rect);
        draw_centered_text(renderer, value_style, dpad_.center_value, text_rect);
    }

    if (dpad_.visible) {
        if (dpad_up_button_) dpad_up_button_->render(renderer);
        if (dpad_down_button_) dpad_down_button_->render(renderer);
        if (dpad_left_button_) dpad_left_button_->render(renderer);
        if (dpad_right_button_) dpad_right_button_->render(renderer);
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
    if (!dpad_up_button_) {
        dpad_up_button_ = std::make_unique<DMButton>("Up", &DMStyles::SecondaryButton(), kDpadButtonWidth, DMButton::height());
    }
    if (!dpad_down_button_) {
        dpad_down_button_ = std::make_unique<DMButton>("Down", &DMStyles::SecondaryButton(), kDpadButtonWidth, DMButton::height());
    }
    if (!dpad_left_button_) {
        dpad_left_button_ = std::make_unique<DMButton>("Left", &DMStyles::SecondaryButton(), kDpadButtonWidth, DMButton::height());
    }
    if (!dpad_right_button_) {
        dpad_right_button_ = std::make_unique<DMButton>("Right", &DMStyles::SecondaryButton(), kDpadButtonWidth, DMButton::height());
    }
}

void BottomNavigationPanel::update_layout() const {
    if (!layout_dirty_) {
        return;
    }

    const_cast<BottomNavigationPanel*>(this)->ensure_widgets();

    if (!visible_ || !dpad_.visible || screen_w_ <= 0 || screen_h_ <= 0) {
        panel_rect_ = SDL_Rect{0, 0, 0, 0};
        const_cast<DpadNavigation&>(dpad_).bounds = SDL_Rect{0, 0, 0, 0};
        const_cast<DpadNavigation&>(dpad_).center_rect = SDL_Rect{0, 0, 0, 0};
        if (dpad_up_button_) dpad_up_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (dpad_down_button_) dpad_down_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (dpad_left_button_) dpad_left_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (dpad_right_button_) dpad_right_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        layout_dirty_ = false;
        return;
    }

    const int button_height = DMButton::height();
    const int max_panel_inner_width = std::max(0, screen_w_ - ((kBottomMargin + kPanelPadding) * 2));
    const int fixed_width = (kDpadButtonWidth * 2) + (kPanelGap * 2);
    int center_width = std::clamp(max_panel_inner_width - fixed_width, kDpadCenterMinWidth, kDpadCenterMaxWidth);
    if (fixed_width + center_width > max_panel_inner_width) {
        center_width = std::max(120, max_panel_inner_width - fixed_width);
    }
    const int content_width = std::max(0, fixed_width + center_width);
    const int content_height = button_height + kPanelGap + kDpadCenterHeight + kPanelGap + button_height;

    panel_rect_.w = std::max(0, content_width + (kPanelPadding * 2));
    panel_rect_.h = std::max(0, content_height + (kPanelPadding * 2));
    panel_rect_.x = std::max(0, (screen_w_ - panel_rect_.w) / 2);
    panel_rect_.y = std::max(0, screen_h_ - panel_rect_.h - kBottomMargin);

    const int content_x = panel_rect_.x + kPanelPadding;
    const int content_y = panel_rect_.y + kPanelPadding;
    const int center_x = content_x + kDpadButtonWidth + kPanelGap;
    const int center_y = content_y + button_height + kPanelGap;
    const int left_button_y = center_y + (kDpadCenterHeight - button_height) / 2;

    const SDL_Rect center_rect{center_x, center_y, center_width, kDpadCenterHeight};
    const SDL_Rect up_rect{
        center_x + std::max(0, (center_width - kDpadButtonWidth) / 2),
        content_y,
        kDpadButtonWidth,
        button_height};
    const SDL_Rect down_rect{
        center_x + std::max(0, (center_width - kDpadButtonWidth) / 2),
        center_y + kDpadCenterHeight + kPanelGap,
        kDpadButtonWidth,
        button_height};
    const SDL_Rect left_rect{content_x, left_button_y, kDpadButtonWidth, button_height};
    const SDL_Rect right_rect{
        center_x + center_width + kPanelGap,
        left_button_y,
        kDpadButtonWidth,
        button_height};

    if (dpad_up_button_) dpad_up_button_->set_rect(up_rect);
    if (dpad_down_button_) dpad_down_button_->set_rect(down_rect);
    if (dpad_left_button_) dpad_left_button_->set_rect(left_rect);
    if (dpad_right_button_) dpad_right_button_->set_rect(right_rect);

    const_cast<DpadNavigation&>(dpad_).center_rect = center_rect;
    const_cast<DpadNavigation&>(dpad_).bounds = SDL_Rect{content_x, content_y, content_width, content_height};
    layout_dirty_ = false;
}

bool BottomNavigationPanel::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point point{x, y};
    return SDL_PointInRect(&point, &rect);
}
