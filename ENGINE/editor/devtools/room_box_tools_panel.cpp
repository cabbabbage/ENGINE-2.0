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
constexpr int kPanelMinHeight = 420;
constexpr int kPanelMaxHeight = 760;
constexpr int kPanelPadding = 12;
constexpr int kSectionGap = 10;
constexpr int kHeaderHeight = 24;
constexpr int kLineHeight = 18;

int parse_int_or(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

const char* corner_label_for_index(int index) {
    static constexpr const char* kLabels[4] = {"TL", "TR", "BL", "BR"};
    const int clamped = std::clamp(index, 0, 3);
    return kLabels[clamped];
}

}  // namespace

RoomBoxToolsPanel::RoomBoxToolsPanel(Kind kind)
    : kind_(kind) {
    const char* add_label = "Add Hit Box";
    const char* enabled_label = "Hit Boxes Enabled";
    switch (kind_) {
        case Kind::HitBox:
            add_label = "Add Hit Box";
            enabled_label = "Hit Boxes Enabled";
            break;
        case Kind::AttackBox:
            add_label = "Add Attack Box";
            enabled_label = "Attack Boxes Enabled";
            break;
        case Kind::ImpassableBox:
            add_label = "Add Impassable Box";
            enabled_label = "Impassable Boxes Enabled";
            break;
    }
    add_button_ = std::make_unique<DMButton>(add_label,
                                             &DMStyles::CreateButton(),
                                             180,
                                             DMButton::height());
    delete_button_ = std::make_unique<DMButton>("Delete", &DMStyles::DeleteButton(), 120, DMButton::height());
    apply_next_frame_button_ = std::make_unique<DMButton>("Copy To Next Frame", &DMStyles::PrimaryButton(), 170, DMButton::height());
    apply_animation_button_ = std::make_unique<DMButton>("Copy To Animation", &DMStyles::PrimaryButton(), 170, DMButton::height());
    apply_asset_button_ = std::make_unique<DMButton>("Copy To Asset", &DMStyles::PrimaryButton(), 170, DMButton::height());
    name_textbox_ = std::make_unique<DMTextBox>("Name", "");
    extrusion_textbox_ = std::make_unique<DMTextBox>("Extrusion", "0");
    damage_textbox_ = std::make_unique<DMTextBox>("Damage", "0");
    system_enabled_checkbox_ = std::make_unique<DMCheckbox>(enabled_label, false);
    onion_skin_checkbox_ = std::make_unique<DMCheckbox>("Show onion skin (prev/next)", false);
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

void RoomBoxToolsPanel::set_name_text(const std::string& value) {
    if (name_textbox_) {
        name_textbox_->set_value(value);
    }
}

void RoomBoxToolsPanel::set_detail_values(const DetailValues& values) {
    if (name_textbox_ && !name_textbox_->is_editing()) {
        name_textbox_->set_value(values.name);
    }
    if (extrusion_textbox_ && !extrusion_textbox_->is_editing()) {
        extrusion_textbox_->set_value(std::to_string(values.extrusion));
    }
    if (damage_textbox_ && !damage_textbox_->is_editing()) {
        damage_textbox_->set_value(std::to_string(values.damage));
    }
}

void RoomBoxToolsPanel::set_onion_skin_enabled(bool enabled) {
    if (onion_skin_checkbox_) {
        onion_skin_checkbox_->set_value(enabled);
    }
}

bool RoomBoxToolsPanel::onion_skin_enabled() const {
    return onion_skin_checkbox_ ? onion_skin_checkbox_->value() : false;
}

void RoomBoxToolsPanel::set_system_enabled(bool enabled) {
    if (system_enabled_checkbox_) {
        system_enabled_checkbox_->set_value(enabled);
    }
}

bool RoomBoxToolsPanel::system_enabled() const {
    return system_enabled_checkbox_ ? system_enabled_checkbox_->value() : false;
}

void RoomBoxToolsPanel::set_propagation_visible(bool visible) {
    if (propagation_visible_ == visible) {
        return;
    }
    propagation_visible_ = visible;
    layout_dirty_ = true;
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

void RoomBoxToolsPanel::set_on_propagate(PropagateCallback callback) {
    on_propagate_ = std::move(callback);
}

void RoomBoxToolsPanel::set_on_onion_skin_toggle(OnionSkinToggleCallback callback) {
    on_onion_skin_toggle_ = std::move(callback);
}

void RoomBoxToolsPanel::set_on_system_enabled_toggle(SystemEnabledToggleCallback callback) {
    on_system_enabled_toggle_ = std::move(callback);
}

RoomBoxToolsPanel::DetailValues RoomBoxToolsPanel::collect_detail_values() const {
    DetailValues values;
    values.name = name_textbox_ ? name_textbox_->value() : std::string{};
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
    const bool wheel_event = event.type == SDL_EVENT_MOUSE_WHEEL;

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

    if (system_enabled_checkbox_) {
        const bool before = system_enabled_checkbox_->value();
        if (system_enabled_checkbox_->handle_event(event)) {
            handled = true;
            const bool after = system_enabled_checkbox_->value();
            if (before != after && on_system_enabled_toggle_) {
                on_system_enabled_toggle_(after);
            }
        }
    }

    if (!system_enabled()) {
        if ((pointer_event || wheel_event) && pointer_inside_panel) {
            return true;
        }
        return handled;
    }

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
    const bool has_selected_box = selected_box_index_ >= 0 &&
                                  selected_box_index_ < static_cast<int>(box_names_.size());

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

    if (onion_skin_checkbox_) {
        const bool before = onion_skin_checkbox_->value();
        if (onion_skin_checkbox_->handle_event(event)) {
            handled = true;
            const bool after = onion_skin_checkbox_->value();
            if (before != after && on_onion_skin_toggle_) {
                on_onion_skin_toggle_(after);
            }
        }
    }

    if (propagation_visible_) {
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
    }

    bool details_changed = false;
    if (has_selected_box && name_textbox_ && name_textbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_box && extrusion_textbox_ && extrusion_textbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_box && kind_ == Kind::AttackBox &&
        damage_textbox_ && damage_textbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (details_changed && on_apply_) {
        on_apply_(collect_detail_values());
        handled = true;
    }

    const bool name_editing = name_textbox_ && name_textbox_->is_editing();
    const bool extrusion_editing = extrusion_textbox_ && extrusion_textbox_->is_editing();
    const bool damage_editing = kind_ == Kind::AttackBox && damage_textbox_ && damage_textbox_->is_editing();
    if ((event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN) &&
        (name_editing || extrusion_editing || damage_editing)) {
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
    const char* title = "Hit Box Editor";
    switch (kind_) {
        case Kind::HitBox:
            title = "Hit Box Editor";
            break;
        case Kind::AttackBox:
            title = "Attack Box Editor";
            break;
        case Kind::ImpassableBox:
            title = "Impassable Box Editor";
            break;
    }
    DMFontCache::instance().draw_text(renderer, label_style, title, header_rect_.x, header_rect_.y);
    const std::string subtitle = selected_box_index_ >= 0 && selected_box_index_ < static_cast<int>(box_names_.size())
                                     ? box_names_[static_cast<std::size_t>(selected_box_index_)]
                                     : "No box selected";
    if (system_enabled_checkbox_) {
        system_enabled_checkbox_->render(renderer);
    }
    if (!system_enabled()) {
        return;
    }
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
    if (onion_skin_checkbox_) {
        onion_skin_checkbox_->render(renderer);
    }
    const bool has_selected_box = selected_box_index_ >= 0 &&
                                  selected_box_index_ < static_cast<int>(box_names_.size());
    if (has_selected_box) {
        DMFontCache::instance().draw_text(renderer, label_style, "Box Properties", detail_title_rect_.x, detail_title_rect_.y);
        DMFontCache::instance().draw_text(renderer,
                                          label_style,
                                          "Selected Corner: " + std::string(corner_label_for_index(selected_corner_index_)),
                                          corner_label_rect_.x,
                                          corner_label_rect_.y);
        if (name_textbox_) {
            name_textbox_->render(renderer);
        }
        if (extrusion_textbox_) {
            extrusion_textbox_->render(renderer);
        }
        if (kind_ == Kind::AttackBox && damage_textbox_) {
            damage_textbox_->render(renderer);
        }
    }
    if (propagation_visible_) {
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
        panel_rect_.h = std::max(panel_rect_.h, kPanelMinHeight);
    }
    panel_rect_.w = std::max(panel_rect_.w, 240);
    panel_rect_.h = std::max(panel_rect_.h, 220);

    header_rect_ = SDL_Rect{
        panel_rect_.x + kPanelPadding,
        panel_rect_.y + kPanelPadding,
        panel_rect_.w - (kPanelPadding * 2),
        kHeaderHeight};
    enabled_toggle_rect_ = SDL_Rect{
        header_rect_.x,
        header_rect_.y + header_rect_.h + 2,
        header_rect_.w,
        DMCheckbox::height()};
    if (system_enabled_checkbox_) {
        system_enabled_checkbox_->set_rect(enabled_toggle_rect_);
    }
    if (!system_enabled()) {
        subtitle_rect_ = SDL_Rect{0, 0, 0, 0};
        list_clip_rect_ = SDL_Rect{0, 0, 0, 0};
        detail_title_rect_ = SDL_Rect{0, 0, 0, 0};
        corner_label_rect_ = SDL_Rect{0, 0, 0, 0};
        if (name_textbox_) {
            name_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (extrusion_textbox_) {
            extrusion_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (damage_textbox_) {
            damage_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (add_button_) {
            add_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (delete_button_) {
            delete_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (onion_skin_checkbox_) {
            onion_skin_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (apply_next_frame_button_) {
            apply_next_frame_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (apply_animation_button_) {
            apply_animation_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (apply_asset_button_) {
            apply_asset_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        layout_dirty_ = false;
        return;
    }
    subtitle_rect_ = SDL_Rect{
        header_rect_.x,
        enabled_toggle_rect_.y + enabled_toggle_rect_.h + 2,
        header_rect_.w,
        20};

    const int controls_x = panel_rect_.x + kPanelPadding;
    const int controls_w = std::max(0, panel_rect_.w - (kPanelPadding * 2));
    const int row_gap = 6;
    const int name_h = name_textbox_ ? name_textbox_->preferred_height(controls_w) : DMTextBox::height();
    const int extrusion_h = extrusion_textbox_ ? extrusion_textbox_->preferred_height(controls_w) : DMTextBox::height();
    const int damage_h = damage_textbox_ ? damage_textbox_->preferred_height(controls_w) : DMTextBox::height();
    const bool has_selected_box = selected_box_index_ >= 0 &&
                                  selected_box_index_ < static_cast<int>(box_names_.size());

    int controls_height = 0;
    controls_height += DMButton::height();                               // add
    controls_height += kSectionGap;
    controls_height += DMButton::height();                               // delete
    controls_height += kSectionGap;
    controls_height += DMCheckbox::height();                             // onion skin
    if (has_selected_box) {
        controls_height += kSectionGap;
        controls_height += kLineHeight;                                  // details title
        controls_height += row_gap;
        controls_height += kLineHeight;                                  // corner label
        controls_height += row_gap;
        controls_height += name_h;                                       // name
        controls_height += row_gap;
        controls_height += extrusion_h;                                  // extrusion
        controls_height += row_gap;
        if (kind_ == Kind::AttackBox) {
            controls_height += damage_h;                                 // damage
            controls_height += row_gap;
        }
    } else {
        controls_height += kSectionGap;
    }
    if (propagation_visible_) {
        controls_height += DMButton::height();                           // copy next
        controls_height += row_gap;
        controls_height += DMButton::height();                           // copy animation
        controls_height += row_gap;
        controls_height += DMButton::height();                           // copy asset
    }

    const int list_top = subtitle_rect_.y + 22;
    const int available_list_height =
        (panel_rect_.y + panel_rect_.h) - kPanelPadding - controls_height - kSectionGap - list_top;
    const int list_height = std::max(0, available_list_height);
    list_clip_rect_ = SDL_Rect{controls_x, list_top, controls_w, list_height};

    int y = list_clip_rect_.y + list_clip_rect_.h + kSectionGap;
    if (add_button_) {
        add_button_->set_rect(SDL_Rect{controls_x, y, controls_w, DMButton::height()});
    }
    y += DMButton::height() + kSectionGap;
    if (delete_button_) {
        delete_button_->set_rect(SDL_Rect{controls_x, y, controls_w, DMButton::height()});
    }
    y += DMButton::height() + kSectionGap;
    if (onion_skin_checkbox_) {
        onion_skin_checkbox_->set_rect(SDL_Rect{controls_x, y, controls_w, DMCheckbox::height()});
    }
    y += DMCheckbox::height() + kSectionGap;

    if (has_selected_box) {
        detail_title_rect_ = SDL_Rect{controls_x, y, controls_w, kLineHeight};
        y += kLineHeight + row_gap;
        corner_label_rect_ = SDL_Rect{controls_x, y, controls_w, kLineHeight};
        y += kLineHeight + row_gap;

        if (name_textbox_) {
            name_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, name_h});
        }
        y += name_h + row_gap;
        if (extrusion_textbox_) {
            extrusion_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, extrusion_h});
        }
        y += extrusion_h + row_gap;
        if (kind_ == Kind::AttackBox && damage_textbox_) {
            damage_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, damage_h});
            y += damage_h + row_gap;
        }
    } else {
        detail_title_rect_ = SDL_Rect{0, 0, 0, 0};
        corner_label_rect_ = SDL_Rect{0, 0, 0, 0};
        if (name_textbox_) {
            name_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (extrusion_textbox_) {
            extrusion_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (damage_textbox_) {
            damage_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
    }

    if (propagation_visible_) {
        if (apply_next_frame_button_) {
            apply_next_frame_button_->set_rect(SDL_Rect{controls_x, y, controls_w, DMButton::height()});
        }
        y += DMButton::height() + row_gap;
        if (apply_animation_button_) {
            apply_animation_button_->set_rect(SDL_Rect{controls_x, y, controls_w, DMButton::height()});
        }
        y += DMButton::height() + row_gap;
        if (apply_asset_button_) {
            apply_asset_button_->set_rect(SDL_Rect{controls_x, y, controls_w, DMButton::height()});
        }
    } else {
        if (apply_next_frame_button_) {
            apply_next_frame_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (apply_animation_button_) {
            apply_animation_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (apply_asset_button_) {
            apply_asset_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
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
