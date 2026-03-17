#include "room_box_tools_panel.hpp"

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
constexpr int kPanelWidth = 320;
constexpr int kPanelMinHeight = 340;
constexpr int kPanelMaxHeight = 620;
constexpr int kPanelPadding = 10;
constexpr int kSectionGap = 8;
constexpr int kHeaderHeight = 24;

int parse_int_or(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

RoomBoxToolsPanel::RoomBoxToolsPanel(Kind kind)
    : kind_(kind) {
    add_button_ = std::make_unique<DMButton>(kind_ == Kind::HitBox ? "Add Hit Box" : "Add Attack Box",
                                             &DMStyles::CreateButton(),
                                             180,
                                             DMButton::height());
    delete_button_ = std::make_unique<DMButton>("Delete", &DMStyles::DeleteButton(), 120, DMButton::height());
    apply_button_ = std::make_unique<DMButton>("Apply Fields", &DMStyles::AccentButton(), 150, DMButton::height());
    name_textbox_ = std::make_unique<DMTextBox>("Name", "");
    corner_x_textbox_ = std::make_unique<DMTextBox>("Corner X", "0");
    corner_y_textbox_ = std::make_unique<DMTextBox>("Corner Y", "0");
    extrusion_textbox_ = std::make_unique<DMTextBox>("Extrusion", "0");
    damage_textbox_ = std::make_unique<DMTextBox>("Damage", "0");
}

RoomBoxToolsPanel::~RoomBoxToolsPanel() = default;

void RoomBoxToolsPanel::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    if (!visible_) {
        scroll_offset_ = 0;
    }
    layout_dirty_ = true;
}

void RoomBoxToolsPanel::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void RoomBoxToolsPanel::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_bounds_override_ = bounds;
    panel_bounds_override_active_ = bounds.w > 0 && bounds.h > 0;
    layout_dirty_ = true;
}

void RoomBoxToolsPanel::clear_panel_bounds_override() {
    panel_bounds_override_active_ = false;
    panel_bounds_override_ = SDL_Rect{0, 0, 0, 0};
    layout_dirty_ = true;
}

void RoomBoxToolsPanel::set_box_names(const std::vector<std::string>& names) {
    if (box_names_ == names) {
        return;
    }
    box_names_ = names;
    box_buttons_.clear();
    box_buttons_.reserve(box_names_.size());
    for (const std::string& name : box_names_) {
        box_buttons_.push_back(std::make_unique<DMButton>(name, &DMStyles::ListButton(), 220, DMButton::height()));
    }
    if (selected_box_index_ < 0 || selected_box_index_ >= static_cast<int>(box_names_.size())) {
        selected_box_index_ = box_names_.empty() ? -1 : 0;
    }
    layout_dirty_ = true;
}

void RoomBoxToolsPanel::set_selection(int box_index, int corner_index) {
    const int bounded_corner = std::clamp(corner_index, 0, 3);
    if (selected_box_index_ == box_index && selected_corner_index_ == bounded_corner) {
        return;
    }
    selected_box_index_ = box_index;
    selected_corner_index_ = bounded_corner;
    layout_dirty_ = true;
}

void RoomBoxToolsPanel::clear_selection() {
    set_selection(-1, 0);
}

void RoomBoxToolsPanel::set_detail_values(const DetailValues& values) {
    if (name_textbox_) {
        name_textbox_->set_value(values.name);
    }
    if (corner_x_textbox_) {
        corner_x_textbox_->set_value(std::to_string(values.corner_x));
    }
    if (corner_y_textbox_) {
        corner_y_textbox_->set_value(std::to_string(values.corner_y));
    }
    if (extrusion_textbox_) {
        extrusion_textbox_->set_value(std::to_string(values.extrusion));
    }
    if (damage_textbox_) {
        damage_textbox_->set_value(std::to_string(values.damage));
    }
}

void RoomBoxToolsPanel::set_on_select(SelectCallback callback) {
    on_select_ = std::move(callback);
}

void RoomBoxToolsPanel::set_on_add(AddCallback callback) {
    on_add_ = std::move(callback);
}

void RoomBoxToolsPanel::set_on_delete(DeleteCallback callback) {
    on_delete_ = std::move(callback);
}

void RoomBoxToolsPanel::set_on_apply(ApplyCallback callback) {
    on_apply_ = std::move(callback);
}

RoomBoxToolsPanel::DetailValues RoomBoxToolsPanel::collect_detail_values() const {
    DetailValues values;
    values.name = name_textbox_ ? name_textbox_->value() : std::string{};
    values.corner_x = parse_int_or(corner_x_textbox_ ? corner_x_textbox_->value() : std::string{}, 0);
    values.corner_y = parse_int_or(corner_y_textbox_ ? corner_y_textbox_->value() : std::string{}, 0);
    values.extrusion = parse_int_or(extrusion_textbox_ ? extrusion_textbox_->value() : std::string{}, 0);
    values.damage = parse_int_or(damage_textbox_ ? damage_textbox_->value() : std::string{}, 0);
    return values;
}

bool RoomBoxToolsPanel::handle_event(const SDL_Event& event) {
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

    layout_box_buttons();

    for (std::size_t i = 0; i < box_buttons_.size(); ++i) {
        DMButton* button = box_buttons_[i].get();
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
                event.button.button == SDL_BUTTON_LEFT) {
                selected_box_index_ = static_cast<int>(i);
                if (on_select_) {
                    on_select_(selected_box_index_);
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

    if (delete_button_ && delete_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_delete_) {
            on_delete_();
        }
    }

    if (apply_button_ && apply_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_apply_) {
            on_apply_(collect_detail_values());
        }
    }

    if (name_textbox_ && name_textbox_->handle_event(event)) {
        handled = true;
    }
    if (corner_x_textbox_ && corner_x_textbox_->handle_event(event)) {
        handled = true;
    }
    if (corner_y_textbox_ && corner_y_textbox_->handle_event(event)) {
        handled = true;
    }
    if (extrusion_textbox_ && extrusion_textbox_->handle_event(event)) {
        handled = true;
    }
    if (kind_ == Kind::AttackBox && damage_textbox_ && damage_textbox_->handle_event(event)) {
        handled = true;
    }

    if (pointer_event && pointer_inside_panel) {
        handled = true;
    }

    return handled;
}

void RoomBoxToolsPanel::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) {
        return;
    }
    const_cast<RoomBoxToolsPanel*>(this)->update_layout();

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
                                      kind_ == Kind::HitBox ? "Hitbox Editor" : "Attack Box Editor",
                                      header_rect_.x,
                                      header_rect_.y);
    const std::string subtitle = selected_box_index_ >= 0
                                     ? ("Corner " + std::to_string(selected_corner_index_ + 1))
                                     : "No selection";
    DMFontCache::instance().draw_text(renderer, label_style, subtitle, subtitle_rect_.x, subtitle_rect_.y);

    SDL_SetRenderDrawColor(renderer, 20, 24, 30, 180);
    sdl_render::FillRect(renderer, &list_clip_rect_);
    dm_draw::DrawRoundedOutline(renderer, list_clip_rect_, 4, 1, DMStyles::Border());

    SDL_Rect previous_clip{};
    SDL_GetRenderClipRect(renderer, &previous_clip);
    const bool was_clipping = SDL_RenderClipEnabled(renderer);
    SDL_SetRenderClipRect(renderer, &list_clip_rect_);
    for (const auto& button : box_buttons_) {
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
    if (delete_button_) {
        delete_button_->render(renderer);
    }
    if (name_textbox_) {
        name_textbox_->render(renderer);
    }
    if (corner_x_textbox_) {
        corner_x_textbox_->render(renderer);
    }
    if (corner_y_textbox_) {
        corner_y_textbox_->render(renderer);
    }
    if (extrusion_textbox_) {
        extrusion_textbox_->render(renderer);
    }
    if (kind_ == Kind::AttackBox && damage_textbox_) {
        damage_textbox_->render(renderer);
    }
    if (apply_button_) {
        apply_button_->render(renderer);
    }
}

bool RoomBoxToolsPanel::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    update_layout();
    return point_in_rect(x, y, panel_rect_);
}

void RoomBoxToolsPanel::update_layout() const {
    if (!layout_dirty_) {
        return;
    }

    if (panel_bounds_override_active_) {
        panel_rect_ = panel_bounds_override_;
    } else {
        const int available_h = std::max(0, screen_h_ - (kTopOffset + 48));
        const int panel_h = std::clamp(available_h, kPanelMinHeight, kPanelMaxHeight);
        panel_rect_ = SDL_Rect{kPanelMargin, kTopOffset, kPanelWidth, panel_h};
    }
    panel_rect_.w = std::max(panel_rect_.w, 240);
    panel_rect_.h = std::max(panel_rect_.h, kPanelMinHeight);

    header_rect_ = SDL_Rect{
        panel_rect_.x + kPanelPadding,
        panel_rect_.y + kPanelPadding,
        panel_rect_.w - (kPanelPadding * 2),
        kHeaderHeight};
    subtitle_rect_ = SDL_Rect{
        header_rect_.x,
        header_rect_.y + header_rect_.h - 2,
        header_rect_.w,
        20};

    int y = subtitle_rect_.y + 22;
    add_button_->set_rect(SDL_Rect{panel_rect_.x + kPanelPadding, y, panel_rect_.w - (kPanelPadding * 2), DMButton::height()});
    y += DMButton::height() + kSectionGap;

    list_clip_rect_ = SDL_Rect{panel_rect_.x + kPanelPadding, y, panel_rect_.w - (kPanelPadding * 2), 144};
    y += list_clip_rect_.h + kSectionGap;

    delete_button_->set_rect(SDL_Rect{panel_rect_.x + kPanelPadding, y, panel_rect_.w - (kPanelPadding * 2), DMButton::height()});
    y += DMButton::height() + kSectionGap;

    detail_title_rect_ = SDL_Rect{panel_rect_.x + kPanelPadding, y, panel_rect_.w - (kPanelPadding * 2), 18};
    y += detail_title_rect_.h + 2;
    corner_label_rect_ = SDL_Rect{detail_title_rect_.x, y, detail_title_rect_.w, 16};
    y += corner_label_rect_.h + 2;

    const SDL_Rect row_rect{panel_rect_.x + kPanelPadding, y, panel_rect_.w - (kPanelPadding * 2), DMTextBox::height()};
    if (name_textbox_) {
        name_textbox_->set_rect(row_rect);
    }
    y += DMTextBox::height() + 2;
    if (corner_x_textbox_) {
        corner_x_textbox_->set_rect(SDL_Rect{row_rect.x, y, row_rect.w, DMTextBox::height()});
    }
    y += DMTextBox::height() + 2;
    if (corner_y_textbox_) {
        corner_y_textbox_->set_rect(SDL_Rect{row_rect.x, y, row_rect.w, DMTextBox::height()});
    }
    y += DMTextBox::height() + 2;
    if (extrusion_textbox_) {
        extrusion_textbox_->set_rect(SDL_Rect{row_rect.x, y, row_rect.w, DMTextBox::height()});
    }
    y += DMTextBox::height() + 2;
    if (kind_ == Kind::AttackBox && damage_textbox_) {
        damage_textbox_->set_rect(SDL_Rect{row_rect.x, y, row_rect.w, DMTextBox::height()});
        y += DMTextBox::height() + 2;
    }
    if (apply_button_) {
        apply_button_->set_rect(SDL_Rect{row_rect.x, y, row_rect.w, DMButton::height()});
    }

    layout_box_buttons();
    layout_dirty_ = false;
}

void RoomBoxToolsPanel::layout_box_buttons() const {
    int y = list_clip_rect_.y - scroll_offset_;
    const int button_h = DMButton::height();
    const int gap = DMSpacing::small_gap();
    for (std::size_t i = 0; i < box_buttons_.size(); ++i) {
        DMButton* button = box_buttons_[i].get();
        if (!button) {
            continue;
        }
        button->set_rect(SDL_Rect{list_clip_rect_.x + 4, y, list_clip_rect_.w - 8, button_h});
        if (static_cast<int>(i) == selected_box_index_) {
            button->set_style(&DMStyles::AccentButton());
        } else {
            button->set_style(&DMStyles::ListButton());
        }
        y += button_h + gap;
    }
    content_height_ = std::max(0, y - (list_clip_rect_.y - scroll_offset_));
    max_scroll_ = std::max(0, content_height_ - list_clip_rect_.h);
    scroll_offset_ = std::clamp(scroll_offset_, 0, max_scroll_);
}

void RoomBoxToolsPanel::scroll_by(int delta) {
    if (max_scroll_ <= 0) {
        scroll_offset_ = 0;
        return;
    }
    scroll_offset_ = std::clamp(scroll_offset_ + delta, 0, max_scroll_);
    layout_box_buttons();
}

bool RoomBoxToolsPanel::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point point{x, y};
    return SDL_PointInRect(&point, &rect);
}

