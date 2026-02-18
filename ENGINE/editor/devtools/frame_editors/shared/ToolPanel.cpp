#include "ToolPanel.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "FloatingDockableManager.hpp"
#include "FloatingPanelLayoutManager.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "utils/input.hpp"
#include "utils/sdl_render_conversions.hpp"

namespace devmode::frame_editors {

namespace {
int default_content_width() {
    return DockableCollapsible::kDefaultFloatingContentWidth + 40;
}

SDL_Rect full_work_area(int w, int h) {
    return SDL_Rect{0, 0, std::max(0, w), std::max(0, h)};
}

class FrameToolDockable final : public DockableCollapsible {
public:
    using DockableCollapsible::DockableCollapsible;

    bool is_point_over_empty_area(const SDL_Point& p) const {
        if (!expanded_) return false;
        if (body_viewport_.w <= 0 || body_viewport_.h <= 0) return false;
        if (!SDL_PointInRect(&p, &body_viewport_)) return false;
        for (const SDL_Rect& rect : widget_bounds_) {
            if (SDL_PointInRect(&p, &rect)) {
                return false;
            }
        }
        return true;
    }

    const SDL_Rect& body_viewport_bounds() const {
        return body_viewport_;
    }

protected:
    void layout_custom_content(int screen_w, int screen_h) const override {
        widget_bounds_.clear();
        for (const auto& row : rows_) {
            for (Widget* w : row) {
                if (w) {
                    widget_bounds_.push_back(w->rect());
                }
            }
        }
    }

private:
    mutable std::vector<SDL_Rect> widget_bounds_;
};
}  // namespace

FrameToolPanel::FrameToolPanel(std::string title, std::string stack_key)
    : panel_(std::make_unique<FrameToolDockable>(std::move(title), true)),
      panel_impl_(static_cast<FrameToolDockable*>(panel_.get())),
      stack_key_(std::move(stack_key)) {
    if (stack_key_.empty()) {
        stack_key_ = "frame_editor_tool_panel";
    }
    if (panel_) {
        panel_->set_close_button_enabled(false);
        panel_->set_scroll_enabled(true);
        panel_->set_padding(DMSpacing::item_gap());
        panel_->set_row_gap(DMSpacing::small_gap());
        panel_->set_col_gap(DMSpacing::small_gap());
        panel_->set_floating_content_width(default_content_width());
        panel_->set_cell_width(default_content_width() - DMSpacing::item_gap() * 2);
        panel_->set_visible(true);
        panel_->set_expanded(true);
    }
}

FrameToolPanel::~FrameToolPanel() {
    if (panel_) {
        panel_->set_visible(false);
        if (registered_with_manager_) {
            FloatingDockableManager::instance().notify_panel_closed(panel_.get());
        }
    }
    panel_impl_ = nullptr;
}

DockableCollapsible* FrameToolPanel::panel() { return panel_.get(); }
const DockableCollapsible* FrameToolPanel::panel() const { return panel_.get(); }

void FrameToolPanel::set_rows(const DockableCollapsible::Rows& rows) const {
    if (!panel_) return;
    panel_->set_rows(rows);
    panel_->reset_scroll();
}

void FrameToolPanel::set_position_if_unset(int x, int y) const {
    if (!panel_ || has_position_) return;
    panel_->set_position(x, y);
    has_position_ = true;
}

void FrameToolPanel::set_work_area(SDL_Rect area) const {
    if (panel_) {
        panel_->set_work_area(area);
    }
}

void FrameToolPanel::update(const Input& input, int screen_w, int screen_h) const {
    if (!panel_) return;
    if (!registered_with_manager_) {
        FloatingDockableManager::instance().open_floating(
            stack_key_,            // name
            panel_.get(),          // panel
            {},                    // close callback
            stack_key_);           // stack key so frame editor panels stay grouped
        registered_with_manager_ = true;
    }
    if (screen_w > 0 && screen_h > 0) {
        panel_->set_work_area(full_work_area(screen_w, screen_h));
    }
    panel_->update(input, screen_w, screen_h);
}

bool FrameToolPanel::handle_event(const SDL_Event& e) const {
    if (!panel_ || !panel_->is_visible()) {
        return false;
    }

    const bool pointer_event =
        (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION);
    SDL_Point pointer{0, 0};
    if (pointer_event) {
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            pointer = SDL_Point{
                static_cast<int>(std::lround(e.motion.x)),
                static_cast<int>(std::lround(e.motion.y))};
        } else {
            pointer = SDL_Point{
                static_cast<int>(std::lround(e.button.x)),
                static_cast<int>(std::lround(e.button.y))};
        }
    }

    if (panel_impl_) {
        if (pointer_event) {
            hover_empty_area_ = panel_impl_->is_point_over_empty_area(pointer);
        }
    } else {
        hover_empty_area_ = false;
    }

    if (dragging_empty_area_) {
        if (pointer_event && e.type == SDL_EVENT_MOUSE_MOTION) {
            const int target_x = pointer.x - drag_offset_.x;
            const int target_y = pointer.y - drag_offset_.y;
            panel_->set_position(target_x, target_y);
            return true;
        }
        if (pointer_event && e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            dragging_empty_area_ = false;
            FloatingPanelLayoutManager::instance().notifyPanelUserMoved(panel_.get());
            return true;
        }
        if (pointer_event && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            return true;
        }
    }

    if (pointer_event && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT &&
        panel_impl_ && panel_impl_->is_point_over_empty_area(pointer)) {
        dragging_empty_area_ = true;
        drag_offset_.x = pointer.x - panel_->rect().x;
        drag_offset_.y = pointer.y - panel_->rect().y;
        FloatingDockableManager::instance().bring_to_front(panel_.get());
        return true;
    }

    return panel_->handle_event(e);
}

void FrameToolPanel::render(SDL_Renderer* renderer) const {
    if (!panel_ || !panel_->is_visible() || !renderer) {
        return;
    }

    panel_->render(renderer);

    if (!hover_empty_area_ || !panel_impl_) {
        return;
    }

    SDL_Rect highlight_rect = panel_impl_->body_viewport_bounds();
    if (highlight_rect.w <= 0 || highlight_rect.h <= 0) {
        return;
    }

    constexpr int glow_padding = 2;
    SDL_Rect glow_rect = highlight_rect;
    glow_rect.x -= glow_padding;
    glow_rect.y -= glow_padding;
    glow_rect.w += glow_padding * 2;
    glow_rect.h += glow_padding * 2;

    SDL_BlendMode prev_blend = SDL_BLENDMODE_BLEND;
    if (SDL_GetRenderDrawBlendMode(renderer, &prev_blend) != 0) {
        prev_blend = SDL_BLENDMODE_BLEND;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_Color fill = DMStyles::HighlightColor();
    fill.a = 40;
    sdl_render::FillRect(renderer, &glow_rect);

    SDL_Color outline = DMStyles::AccentButton().hover_bg;
    outline.a = 160;
    dm_draw::DrawRoundedOutline(renderer, glow_rect, DMStyles::CornerRadius(), 2, outline);

    SDL_SetRenderDrawBlendMode(renderer, prev_blend);
}

bool FrameToolPanel::contains_point(const SDL_Point& p) const {
    return panel_ ? panel_->is_point_inside(p.x, p.y) : false;
}

}  // namespace devmode::frame_editors
