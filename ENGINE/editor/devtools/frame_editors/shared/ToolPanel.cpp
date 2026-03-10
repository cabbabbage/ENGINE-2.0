#include "ToolPanel.hpp"

#include <algorithm>
#include <utility>

#include <SDL3/SDL.h>

#include "DockManager.hpp"
#include "devtools/dm_styles.hpp"
#include "utils/input.hpp"

namespace devmode::frame_editors {

namespace {
int default_content_width() {
    return DockableCollapsible::kDefaultFloatingContentWidth + 40;
}

SDL_Rect full_work_area(int w, int h) {
    return SDL_Rect{0, 0, std::max(0, w), std::max(0, h)};
}
}  // namespace

class FrameToolDockable final : public DockableCollapsible {
public:
    using DockableCollapsible::DockableCollapsible;

    // Returns true if the point is on the panel rect but NOT on any
    // interactive widget. This includes the body padding, gaps between
    // widgets, and the header area (which DockableCollapsible's own drag
    // also handles, but we want a consistent experience).
    bool is_point_in_non_widget_area(const SDL_Point& p) const {
        if (!SDL_PointInRect(&p, &rect_)) return false;
        // Check each widget rect directly (avoids stale cache issues).
        for (const auto& row : rows_) {
            for (Widget* w : row) {
                if (!w) continue;
                SDL_Rect wr = w->rect();
                if (wr.w > 0 && wr.h > 0 && SDL_PointInRect(&p, &wr)) {
                    return false;
                }
            }
        }
        // Don't hijack close/lock button clicks.
        if (close_btn_) {
            SDL_Rect cr = close_btn_->rect();
            if (cr.w > 0 && cr.h > 0 && SDL_PointInRect(&p, &cr)) return false;
        }
        if (lock_btn_) {
            SDL_Rect lr = lock_btn_->rect();
            if (lr.w > 0 && lr.h > 0 && SDL_PointInRect(&p, &lr)) return false;
        }
        if (header_btn_) {
            SDL_Rect hr = header_btn_->rect();
            if (hr.w > 0 && hr.h > 0 && SDL_PointInRect(&p, &hr)) return false;
        }
        return true;
    }

    bool is_point_on_panel(const SDL_Point& p) const {
        return SDL_PointInRect(&p, &rect_);
    }
};

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
        DockManager::instance().notify_panel_closed(panel_.get());
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

void FrameToolPanel::set_position_if_unset(int screen_w, int y) const {
    if (!panel_ || has_position_) return;
    if (screen_w <= 0) return;  // need valid screen width to position on right
    const int panel_w = std::max(panel_->rect().w, default_content_width());
    const int x = screen_w - panel_w - DMSpacing::item_gap();
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
        DockManager::instance().open_floating(
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

    // ── Active drag ──────────────────────────────────────────────────
    if (dragging_empty_area_) {
        if (pointer_event && e.type == SDL_EVENT_MOUSE_MOTION) {
            panel_->set_position(pointer.x - drag_offset_.x,
                                 pointer.y - drag_offset_.y);
            return true;
        }
        if (pointer_event && e.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            e.button.button == SDL_BUTTON_LEFT) {
            dragging_empty_area_ = false;
            DockManager::instance().notifyPanelUserMoved(panel_.get());
            return true;
        }
        if (pointer_event) return true;  // swallow stray events while dragging
    }

    // ── Start drag: mouse-down on any non-widget part of the panel ──
    if (pointer_event && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        e.button.button == SDL_BUTTON_LEFT && panel_impl_ &&
        panel_impl_->is_point_in_non_widget_area(pointer)) {
        dragging_empty_area_ = true;
        drag_offset_.x = pointer.x - panel_->rect().x;
        drag_offset_.y = pointer.y - panel_->rect().y;
        DockManager::instance().bring_to_front(panel_.get());
        return true;
    }

    // ── Forward to the underlying DockableCollapsible (widgets, scroll, etc.)
    if (panel_->handle_event(e)) {
        return true;
    }

    // ── Consume all remaining pointer events that land on the panel ──
    // This prevents clicks/motion from bleeding through to the canvas.
    if (pointer_event && panel_impl_ && panel_impl_->is_point_on_panel(pointer)) {
        return true;
    }

    if (e.type == SDL_EVENT_MOUSE_WHEEL && panel_impl_) {
        float mx = 0.0f;
        float my = 0.0f;
        SDL_GetMouseState(&mx, &my);
        SDL_Point wheel_pos{
            static_cast<int>(std::lround(mx)),
            static_cast<int>(std::lround(my))};
        if (panel_impl_->is_point_on_panel(wheel_pos)) {
            return true;
        }
    }

    return false;
}

void FrameToolPanel::render(SDL_Renderer* renderer) const {
    if (!panel_ || !panel_->is_visible() || !renderer) {
        return;
    }
    panel_->render(renderer);
}

bool FrameToolPanel::contains_point(const SDL_Point& p) const {
    return panel_ ? panel_->is_point_inside(p.x, p.y) : false;
}

}  // namespace devmode::frame_editors
