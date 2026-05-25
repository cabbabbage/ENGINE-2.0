#include "asset_stack_animation_list_panel.hpp"

#include <algorithm>

#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "utils/sdl_mouse_utils.hpp"

namespace {

constexpr int kPanelMargin = 12;
constexpr int kRowHeight = 46;
constexpr int kRowGap = 6;
constexpr int kInnerPadding = 10;

int wheel_delta(const SDL_MouseWheelEvent& wheel) {
    int delta = wheel.integer_y;
    if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        delta = -delta;
    }
    if (delta == 0) {
        float precise = wheel.y;
        if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            precise = -precise;
        }
        if (precise > 0.0f) delta = 1;
        if (precise < 0.0f) delta = -1;
    }
    return delta;
}

}  // namespace

AssetStackAnimationListPanel::AssetStackAnimationListPanel() = default;

void AssetStackAnimationListPanel::set_visible(bool visible) {
    visible_ = visible;
}

void AssetStackAnimationListPanel::set_screen_dimensions(int width, int height) {
    screen_w_ = std::max(0, width);
    screen_h_ = std::max(0, height);
    if (!panel_override_active_) {
        const int panel_w = std::min(320, std::max(220, screen_w_ / 4));
        const int panel_h = std::max(0, screen_h_ - (kPanelMargin * 2));
        panel_rect_ = SDL_Rect{kPanelMargin, kPanelMargin, panel_w, panel_h};
    }
    recalculate_scroll_range();
}

void AssetStackAnimationListPanel::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_override_ = bounds;
    panel_override_active_ = bounds.w > 0 && bounds.h > 0;
    panel_rect_ = panel_override_active_ ? panel_override_ : SDL_Rect{0, 0, 0, 0};
    recalculate_scroll_range();
}

void AssetStackAnimationListPanel::clear_panel_bounds_override() {
    panel_override_active_ = false;
    panel_override_ = SDL_Rect{0, 0, 0, 0};
    const int panel_w = std::min(320, std::max(220, screen_w_ / 4));
    const int panel_h = std::max(0, screen_h_ - (kPanelMargin * 2));
    panel_rect_ = SDL_Rect{kPanelMargin, kPanelMargin, panel_w, panel_h};
    recalculate_scroll_range();
}

void AssetStackAnimationListPanel::set_rows(std::vector<Row> rows) {
    rows_ = std::move(rows);
    recalculate_scroll_range();
}

void AssetStackAnimationListPanel::set_selected_animation_id(const std::optional<std::string>& animation_id) {
    selected_animation_id_ = animation_id;
}

void AssetStackAnimationListPanel::set_on_select(std::function<void(const std::string&)> callback) {
    on_select_ = std::move(callback);
}

bool AssetStackAnimationListPanel::handle_event(const SDL_Event& e) {
    if (!visible_ || panel_rect_.w <= 0 || panel_rect_.h <= 0) {
        return false;
    }

    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        int mx = 0;
        int my = 0;
        sdl_mouse_util::GetMouseState(&mx, &my);
        if (!is_point_inside(mx, my)) {
            return false;
        }
        const int delta = wheel_delta(e.wheel);
        if (delta == 0) {
            return false;
        }
        const int prev = scroll_px_;
        scroll_px_ = std::clamp(scroll_px_ - (delta * 40), 0, max_scroll_px_);
        return scroll_px_ != prev;
    }

    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        hovered_row_ = row_index_at(p);
        return hovered_row_ >= 0 || is_point_inside(p.x, p.y);
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        const int index = row_index_at(p);
        if (index < 0) {
            return is_point_inside(p.x, p.y);
        }
        if (index >= static_cast<int>(rows_.size())) {
            return true;
        }
        const Row& row = rows_[static_cast<std::size_t>(index)];
        if (row.editable && on_select_) {
            on_select_(row.animation_id);
        }
        return true;
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        return is_point_inside(p.x, p.y);
    }

    return false;
}

void AssetStackAnimationListPanel::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer || panel_rect_.w <= 0 || panel_rect_.h <= 0) {
        return;
    }

    const SDL_Color panel_bg = DMStyles::PanelBG();
    dm_draw::DrawBeveledRect(renderer,
                             panel_rect_,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             panel_bg,
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, panel_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());

    const int content_x = panel_rect_.x + kInnerPadding;
    const int content_y = panel_rect_.y + kInnerPadding;
    const int content_w = std::max(0, panel_rect_.w - (kInnerPadding * 2));
    const int content_h = std::max(0, panel_rect_.h - (kInnerPadding * 2));
    if (content_w <= 0 || content_h <= 0) {
        return;
    }

    SDL_Rect clip_rect{content_x, content_y, content_w, content_h};
    SDL_Rect previous_clip{0, 0, 0, 0};
    const bool had_clip = SDL_RenderClipEnabled(renderer);
    if (had_clip) {
        SDL_GetRenderClipRect(renderer, &previous_clip);
    }
    SDL_SetRenderClipRect(renderer, &clip_rect);

    DMLabelStyle title_style = DMStyles::Label();
    title_style.font_size = 14;
    title_style.color = DMStyles::ButtonFocusOutline();
    DMFontCache::instance().draw_text(renderer, title_style, "Animations", content_x, content_y);

    int row_origin_y = content_y + 22 - scroll_px_;

    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        SDL_Rect row_rect_abs{
            content_x,
            row_origin_y + static_cast<int>(i) * (kRowHeight + kRowGap),
            content_w,
            kRowHeight};
        if (row_rect_abs.y + row_rect_abs.h < content_y || row_rect_abs.y > content_y + content_h) {
            continue;
        }

        const bool selected = selected_animation_id_ && *selected_animation_id_ == row.animation_id;
        const bool hovered = hovered_row_ == static_cast<int>(i);

        SDL_Color bg = row.editable ? DMStyles::ListButton().bg : dm_draw::DarkenColor(DMStyles::ListButton().bg, 0.35f);
        if (hovered && row.editable) {
            bg = DMStyles::ListButton().hover_bg;
        }
        dm_draw::DrawBeveledRect(renderer,
                                 row_rect_abs,
                                 DMStyles::CornerRadius(),
                                 1,
                                 bg,
                                 DMStyles::HighlightColor(),
                                 DMStyles::ShadowColor(),
                                 false,
                                 DMStyles::HighlightIntensity() * 0.8f,
                                 DMStyles::ShadowIntensity() * 0.8f);
        SDL_Color border = selected ? DMStyles::AccentButton().bg : DMStyles::Border();
        dm_draw::DrawRoundedOutline(renderer, row_rect_abs, DMStyles::CornerRadius(), selected ? 2 : 1, border);

        DMLabelStyle main_style = DMStyles::Label();
        main_style.font_size = 13;
        if (!row.editable) {
            main_style.color = dm_draw::DarkenColor(main_style.color, 0.35f);
        }
        const int label_x = row_rect_abs.x + 10;
        const int label_y = row_rect_abs.y + 7;
        DMFontCache::instance().draw_text(renderer, main_style, row.animation_id, label_x, label_y);

        DMLabelStyle subtitle_style = DMStyles::Label();
        subtitle_style.font_size = 11;
        subtitle_style.color = row.editable
            ? dm_draw::DarkenColor(DMStyles::ButtonFocusOutline(), 0.15f)
            : dm_draw::DarkenColor(DMStyles::Label().color, 0.45f);
        DMFontCache::instance().draw_text(renderer, subtitle_style, row_subtitle(row), label_x, row_rect_abs.y + 24);
    }

    if (had_clip) {
        SDL_SetRenderClipRect(renderer, &previous_clip);
    } else {
        SDL_SetRenderClipRect(renderer, nullptr);
    }
}

bool AssetStackAnimationListPanel::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    SDL_Point p{x, y};
    return SDL_PointInRect(&p, &panel_rect_);
}

SDL_Rect AssetStackAnimationListPanel::row_rect(int index) const {
    const int content_x = panel_rect_.x + kInnerPadding;
    const int content_y = panel_rect_.y + kInnerPadding + 22 - scroll_px_;
    const int content_w = std::max(0, panel_rect_.w - (kInnerPadding * 2));
    return SDL_Rect{
        content_x,
        content_y + index * (kRowHeight + kRowGap),
        content_w,
        kRowHeight};
}

int AssetStackAnimationListPanel::row_index_at(SDL_Point p) const {
    if (!is_point_inside(p.x, p.y)) {
        return -1;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const SDL_Rect rect = row_rect(static_cast<int>(i));
        if (SDL_PointInRect(&p, &rect)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void AssetStackAnimationListPanel::recalculate_scroll_range() {
    const int content_h = std::max(0, panel_rect_.h - (kInnerPadding * 2));
    const int rows_h = static_cast<int>(rows_.size()) * (kRowHeight + kRowGap);
    const int total_h = 22 + rows_h;
    max_scroll_px_ = std::max(0, total_h - content_h);
    scroll_px_ = std::clamp(scroll_px_, 0, max_scroll_px_);
}

std::string AssetStackAnimationListPanel::row_subtitle(const Row& row) {
    switch (row.reason) {
        case devmode::StackAnimationEditabilityReason::Editable:
            return "editable";
        case devmode::StackAnimationEditabilityReason::InheritedFromAnimationSource:
            return "inherited";
        case devmode::StackAnimationEditabilityReason::MissingFrames:
            return "missing frames";
        case devmode::StackAnimationEditabilityReason::MissingAnimationSource:
            return "missing source";
        case devmode::StackAnimationEditabilityReason::CyclicAnimationSource:
            return "source cycle";
    }
    return "unavailable";
}
