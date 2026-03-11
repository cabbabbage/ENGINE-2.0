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
constexpr int kPanelMinHeight = 260;
constexpr int kPanelMaxHeight = 560;
constexpr int kPanelPadding = 10;
constexpr int kSectionGap = 8;
constexpr int kHeaderHeight = 24;

}  // namespace

RoomAnchorToolsPanel::RoomAnchorToolsPanel() {
    add_button_ = std::make_unique<DMButton>("Add Anchor", &DMStyles::CreateButton(), 180, DMButton::height());
    rename_textbox_ = std::make_unique<DMTextBox>("Rename", "");
    rename_button_ = std::make_unique<DMButton>("Rename", &DMStyles::PrimaryButton(), 120, DMButton::height());
    delete_button_ = std::make_unique<DMButton>("Delete", &DMStyles::DeleteButton(), 120, DMButton::height());
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

    if (rename_textbox_ && rename_textbox_->handle_event(event)) {
        handled = true;
    }

    if (rename_button_ && rename_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_rename_) {
            on_rename_(rename_textbox_->value());
        }
    }

    if (delete_button_ && delete_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_delete_) {
            on_delete_();
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
    if (rename_textbox_) {
        rename_textbox_->render(renderer);
    }
    if (rename_button_) {
        rename_button_->render(renderer);
    }
    if (delete_button_) {
        delete_button_->render(renderer);
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
    panel_rect_ = SDL_Rect{kPanelMargin, kTopOffset, panel_w, panel_h};

    header_rect_ = SDL_Rect{
        panel_rect_.x + kPanelPadding,
        panel_rect_.y + kPanelPadding,
        std::max(0, panel_rect_.w - kPanelPadding * 2),
        kHeaderHeight};

    const int controls_height =
        DMButton::height() +
        kSectionGap +
        DMTextBox::height() +
        kSectionGap +
        DMButton::height();

    const int list_top = header_rect_.y + header_rect_.h + kSectionGap;
    const int bottom_padding = kPanelPadding;
    const int list_height = std::max(72,
                                     panel_rect_.y + panel_rect_.h -
                                         bottom_padding -
                                         controls_height -
                                         (kSectionGap * 2) -
                                         list_top);

    list_clip_rect_ = SDL_Rect{
        panel_rect_.x + kPanelPadding,
        list_top,
        std::max(0, panel_rect_.w - kPanelPadding * 2),
        list_height};

    const int controls_width = std::max(0, panel_rect_.w - kPanelPadding * 2);
    const int controls_x = panel_rect_.x + kPanelPadding;
    const int add_y = list_clip_rect_.y + list_clip_rect_.h + kSectionGap;
    const int rename_y = add_y + DMButton::height() + kSectionGap;
    const int row_y = rename_y + DMTextBox::height() + kSectionGap;

    if (add_button_) {
        add_button_->set_rect(SDL_Rect{controls_x, add_y, controls_width, DMButton::height()});
    }
    if (rename_textbox_) {
        rename_textbox_->set_rect(SDL_Rect{controls_x, rename_y, controls_width, DMTextBox::height()});
    }
    const int row_gap = kSectionGap;
    const int half_width = std::max(64, (controls_width - row_gap) / 2);
    if (rename_button_) {
        rename_button_->set_rect(SDL_Rect{controls_x, row_y, half_width, DMButton::height()});
    }
    if (delete_button_) {
        delete_button_->set_rect(SDL_Rect{controls_x + half_width + row_gap,
                                          row_y,
                                          std::max(64, controls_width - half_width - row_gap),
                                          DMButton::height()});
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
