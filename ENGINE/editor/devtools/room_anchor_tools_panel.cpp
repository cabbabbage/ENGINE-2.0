#include "room_anchor_tools_panel.hpp"

#include <algorithm>
#include <utility>

#include "dm_styles.hpp"
#include "draw_utils.hpp"
#include "font_cache.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "widgets.hpp"

namespace {

constexpr int kPanelMargin = 12;
constexpr int kTopOffset = 56;
constexpr int kPanelWidth = 300;
constexpr int kPanelMinHeight = 340;
constexpr int kPanelMaxHeight = 720;
constexpr int kPanelPadding = 10;
constexpr int kSectionGap = 8;
constexpr int kHeaderHeight = 24;

}  // namespace

RoomAnchorToolsPanel::RoomAnchorToolsPanel() {
    add_button_ = std::make_unique<DMButton>("Add Anchor", &DMStyles::CreateButton(), 180, DMButton::height());
    rename_textbox_ = std::make_unique<DMTextBox>("Rename", "");
    delete_button_ = std::make_unique<DMButton>("Delete", &DMStyles::DeleteButton(), 120, DMButton::height());
    apply_next_frame_button_ = std::make_unique<DMButton>("Copy To Next Frame", &DMStyles::PrimaryButton(), 170, DMButton::height());
    apply_animation_button_ = std::make_unique<DMButton>("Copy To Animation", &DMStyles::PrimaryButton(), 170, DMButton::height());
    apply_asset_button_ = std::make_unique<DMButton>("Copy To Asset", &DMStyles::PrimaryButton(), 170, DMButton::height());
}

RoomAnchorToolsPanel::~RoomAnchorToolsPanel() = default;

void RoomAnchorToolsPanel::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    if (!visible_) {
        scroll_offset_ = 0;
    }
    layout_dirty_ = true;
}

void RoomAnchorToolsPanel::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void RoomAnchorToolsPanel::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_bounds_override_ = bounds;
    panel_bounds_override_active_ = bounds.w > 0 && bounds.h > 0;
    layout_dirty_ = true;
}

void RoomAnchorToolsPanel::clear_panel_bounds_override() {
    panel_bounds_override_active_ = false;
    panel_bounds_override_ = SDL_Rect{0, 0, 0, 0};
    layout_dirty_ = true;
}

void RoomAnchorToolsPanel::set_anchor_names(const std::vector<std::string>& names) {
    if (anchor_names_ == names) {
        return;
    }
    anchor_names_ = names;
    anchor_buttons_.clear();
    anchor_buttons_.reserve(anchor_names_.size());
    for (const std::string& name : anchor_names_) {
        anchor_buttons_.push_back(std::make_unique<DMButton>(name, &DMStyles::ListButton(), 220, DMButton::height()));
    }
    if (!selected_anchor_name_.empty()) {
        const bool still_present = std::find(anchor_names_.begin(),
                                             anchor_names_.end(),
                                             selected_anchor_name_) != anchor_names_.end();
        if (!still_present) {
            selected_anchor_name_.clear();
        }
    }
    layout_dirty_ = true;
}

void RoomAnchorToolsPanel::set_selected_anchor(const std::string& name) {
    if (selected_anchor_name_ == name) {
        return;
    }
    selected_anchor_name_ = name;
    if (!rename_textbox_->is_editing()) {
        rename_textbox_->set_value(selected_anchor_name_);
    }
    layout_dirty_ = true;
}

void RoomAnchorToolsPanel::set_rename_text(const std::string& value) {
    rename_textbox_->set_value(value);
}

std::string RoomAnchorToolsPanel::rename_text() const {
    return rename_textbox_ ? rename_textbox_->value() : std::string{};
}

void RoomAnchorToolsPanel::set_on_select(SelectCallback callback) {
    on_select_ = std::move(callback);
}

void RoomAnchorToolsPanel::set_on_add(AddCallback callback) {
    on_add_ = std::move(callback);
}

void RoomAnchorToolsPanel::set_on_rename(RenameCallback callback) {
    on_rename_ = std::move(callback);
}

void RoomAnchorToolsPanel::set_on_delete(DeleteCallback callback) {
    on_delete_ = std::move(callback);
}

void RoomAnchorToolsPanel::set_on_propagate(PropagateCallback callback) {
    on_propagate_ = std::move(callback);
}

bool RoomAnchorToolsPanel::handle_event(const SDL_Event& event) {
    if (!visible_) {
        return false;
    }
    update_layout();

    int pointer_x = 0;
    int pointer_y = 0;
    const bool pointer_event =
        event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP;

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        pointer_x = event.motion.x;
        pointer_y = event.motion.y;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        pointer_x = event.button.x;
        pointer_y = event.button.y;
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        sdl_mouse_util::GetMouseState(&pointer_x, &pointer_y);
    }

    const bool pointer_inside_panel = point_in_rect(pointer_x, pointer_y, panel_rect_);
    bool handled = false;

    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        if (point_in_rect(pointer_x, pointer_y, list_clip_rect_)) {
            const int step = DMButton::height() + DMSpacing::small_gap();
            scroll_by(-event.wheel.integer_y * step);
        }
        if (pointer_inside_panel) {
            handled = true;
        }
    }

    layout_anchor_buttons();
    const bool has_selected_anchor = !selected_anchor_name_.empty();

    for (std::size_t i = 0; i < anchor_buttons_.size(); ++i) {
        DMButton* button = anchor_buttons_[i].get();
        if (!button) {
            continue;
        }
        const SDL_Rect row_rect = button->rect();
        const bool row_visible = row_rect.y + row_rect.h >= list_clip_rect_.y &&
                                 row_rect.y <= list_clip_rect_.y + list_clip_rect_.h;
        if (!row_visible) {
            continue;
        }
        if (button->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                event.button.button == SDL_BUTTON_LEFT &&
                i < anchor_names_.size()) {
                selected_anchor_name_ = anchor_names_[i];
                if (!rename_textbox_->is_editing()) {
                    rename_textbox_->set_value(selected_anchor_name_);
                }
                if (on_select_) {
                    on_select_(selected_anchor_name_);
                }
            }
        }
    }

    if (add_button_ && add_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_add_) {
            on_add_();
        }
    }

    bool rename_changed = false;
    if (has_selected_anchor && rename_textbox_ && rename_textbox_->handle_event(event)) {
        handled = true;
        rename_changed = true;
    }
    if (rename_changed && on_rename_) {
        on_rename_(rename_textbox_->value());
        handled = true;
    }

    if (delete_button_ && delete_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_delete_) {
            on_delete_();
        }
    }

    if (apply_next_frame_button_ && apply_next_frame_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_propagate_) {
            on_propagate_(PropagationScope::NextFrame);
        }
    }

    if (apply_animation_button_ && apply_animation_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_propagate_) {
            on_propagate_(PropagationScope::Animation);
        }
    }

    if (apply_asset_button_ && apply_asset_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_propagate_) {
            on_propagate_(PropagationScope::Asset);
        }
    }

    if ((event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN) &&
        rename_textbox_ && rename_textbox_->is_editing()) {
        handled = true;
    }

    if (pointer_event && pointer_inside_panel) {
        handled = true;
    }

    return handled;
}

void RoomAnchorToolsPanel::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) {
        return;
    }
    const_cast<RoomAnchorToolsPanel*>(this)->update_layout();

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_Color bg = DMStyles::PanelBG();
    dm_draw::DrawBeveledRect(renderer,
                             panel_rect_,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             bg,
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, panel_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());

    const DMLabelStyle& label_style = DMStyles::Label();
    DMFontCache::instance().draw_text(renderer,
                                      label_style,
                                      "Anchor Tools",
                                      header_rect_.x,
                                      header_rect_.y);

    SDL_SetRenderDrawColor(renderer, 20, 24, 30, 180);
    sdl_render::FillRect(renderer, &list_clip_rect_);
    dm_draw::DrawRoundedOutline(renderer, list_clip_rect_, 4, 1, DMStyles::Border());

    SDL_Rect previous_clip{};
    SDL_GetRenderClipRect(renderer, &previous_clip);
    const bool was_clipping = SDL_RenderClipEnabled(renderer);
    SDL_SetRenderClipRect(renderer, &list_clip_rect_);
    for (const auto& button : anchor_buttons_) {
        if (button) {
            button->render(renderer);
        }
    }
    if (was_clipping) {
        SDL_SetRenderClipRect(renderer, &previous_clip);
    } else {
        SDL_SetRenderClipRect(renderer, nullptr);
    }

    if (add_button_) {
        add_button_->render(renderer);
    }
    const bool has_selected_anchor = !selected_anchor_name_.empty();
    if (has_selected_anchor && rename_textbox_) {
        rename_textbox_->render(renderer);
    }
    if (delete_button_) {
        delete_button_->render(renderer);
    }
    if (apply_next_frame_button_) {
        apply_next_frame_button_->render(renderer);
    }
    if (apply_animation_button_) {
        apply_animation_button_->render(renderer);
    }
    if (apply_asset_button_) {
        apply_asset_button_->render(renderer);
    }
}

bool RoomAnchorToolsPanel::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    const_cast<RoomAnchorToolsPanel*>(this)->update_layout();
    return point_in_rect(x, y, panel_rect_);
}

void RoomAnchorToolsPanel::update_layout() const {
    if (!layout_dirty_) {
        return;
    }

    const int safe_w = std::max(screen_w_, 320);
    const int safe_h = std::max(screen_h_, 320);

    const int panel_w = std::min(kPanelWidth, std::max(220, safe_w - kPanelMargin * 2));
    const int available_h = std::max(kPanelMinHeight, safe_h - kTopOffset - kPanelMargin);
    const int panel_h = std::clamp(available_h, kPanelMinHeight, kPanelMaxHeight);
    if (panel_bounds_override_active_) {
        panel_rect_ = panel_bounds_override_;
    } else {
        panel_rect_ = SDL_Rect{kPanelMargin, kTopOffset, panel_w, panel_h};
    }

    header_rect_ = SDL_Rect{
        panel_rect_.x + kPanelPadding,
        panel_rect_.y + kPanelPadding,
        std::max(0, panel_rect_.w - kPanelPadding * 2),
        kHeaderHeight};

    const int list_top = header_rect_.y + header_rect_.h + kSectionGap;
    const int row_gap = 2;
    const bool has_selected_anchor = !selected_anchor_name_.empty();
    const int controls_width = std::max(0, panel_rect_.w - kPanelPadding * 2);
    const int rename_h = rename_textbox_ ? rename_textbox_->preferred_height(controls_width) : DMTextBox::height();
    int controls_height = 0;
    controls_height += DMButton::height();                          // add
    controls_height += kSectionGap;
    if (has_selected_anchor) {
        controls_height += rename_h;                                // rename text
        controls_height += kSectionGap;
    }
    controls_height += DMButton::height();                          // delete
    controls_height += kSectionGap;
    controls_height += DMButton::height();                          // copy next
    controls_height += row_gap;
    controls_height += DMButton::height();                          // copy animation
    controls_height += row_gap;
    controls_height += DMButton::height();                          // copy asset

    const int list_height = std::max(0,
                                     panel_rect_.y + panel_rect_.h -
                                         kPanelPadding -
                                         controls_height -
                                         kSectionGap -
                                         list_top);

    list_clip_rect_ = SDL_Rect{
        panel_rect_.x + kPanelPadding,
        list_top,
        std::max(0, panel_rect_.w - kPanelPadding * 2),
        list_height};

    const int controls_x = panel_rect_.x + kPanelPadding;
    const int add_y = list_clip_rect_.y + list_clip_rect_.h + kSectionGap;
    int row_y = add_y + DMButton::height() + kSectionGap;

    if (add_button_) {
        add_button_->set_rect(SDL_Rect{controls_x, add_y, controls_width, DMButton::height()});
    }
    if (rename_textbox_) {
        if (has_selected_anchor) {
            rename_textbox_->set_rect(SDL_Rect{controls_x, row_y, controls_width, rename_h});
            row_y += rename_h + kSectionGap;
        } else {
            rename_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
    }
    if (delete_button_) {
        delete_button_->set_rect(SDL_Rect{controls_x, row_y, controls_width, DMButton::height()});
    }

    int copy_y = row_y + DMButton::height() + kSectionGap;
    if (apply_next_frame_button_) {
        apply_next_frame_button_->set_rect(SDL_Rect{controls_x, copy_y, controls_width, DMButton::height()});
    }
    copy_y += DMButton::height() + row_gap;
    if (apply_animation_button_) {
        apply_animation_button_->set_rect(SDL_Rect{controls_x, copy_y, controls_width, DMButton::height()});
    }
    copy_y += DMButton::height() + row_gap;
    if (apply_asset_button_) {
        apply_asset_button_->set_rect(SDL_Rect{controls_x, copy_y, controls_width, DMButton::height()});
    }

    content_height_ = 0;
    if (!anchor_buttons_.empty()) {
        content_height_ = static_cast<int>(anchor_buttons_.size()) * (DMButton::height() + DMSpacing::small_gap()) -
                          DMSpacing::small_gap();
    }
    max_scroll_ = std::max(0, content_height_ - list_clip_rect_.h);
    scroll_offset_ = std::clamp(scroll_offset_, 0, max_scroll_);

    const_cast<RoomAnchorToolsPanel*>(this)->layout_anchor_buttons();
    layout_dirty_ = false;
}

void RoomAnchorToolsPanel::layout_anchor_buttons() const {
    const int button_w = std::max(0, list_clip_rect_.w);
    int y = list_clip_rect_.y - scroll_offset_;
    for (std::size_t i = 0; i < anchor_buttons_.size(); ++i) {
        DMButton* button = anchor_buttons_[i].get();
        if (!button) {
            continue;
        }
        button->set_rect(SDL_Rect{list_clip_rect_.x, y, button_w, DMButton::height()});
        const bool selected = (i < anchor_names_.size() && anchor_names_[i] == selected_anchor_name_);
        button->set_style(selected ? &DMStyles::AccentButton() : &DMStyles::ListButton());
        y += DMButton::height() + DMSpacing::small_gap();
    }
}

void RoomAnchorToolsPanel::scroll_by(int delta) {
    if (delta == 0) {
        return;
    }
    scroll_offset_ = std::clamp(scroll_offset_ + delta, 0, max_scroll_);
    layout_anchor_buttons();
}

bool RoomAnchorToolsPanel::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point point{x, y};
    return SDL_PointInRect(&point, &rect);
}
