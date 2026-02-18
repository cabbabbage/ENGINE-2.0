#include "ToolPanel.hpp"

#include <algorithm>
#include <utility>

#include <SDL3/SDL.h>

#include "FloatingDockableManager.hpp"
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

FrameToolPanel::FrameToolPanel(std::string title, std::string stack_key)
    : panel_(std::make_unique<DockableCollapsible>(std::move(title), true)),
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
    }
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
    return panel_ ? panel_->handle_event(e) : false;
}

void FrameToolPanel::render(SDL_Renderer* renderer) const {
    if (panel_ && panel_->is_visible()) {
        panel_->render(renderer);
    }
}

bool FrameToolPanel::contains_point(const SDL_Point& p) const {
    return panel_ ? panel_->is_point_inside(p.x, p.y) : false;
}

}  // namespace devmode::frame_editors
