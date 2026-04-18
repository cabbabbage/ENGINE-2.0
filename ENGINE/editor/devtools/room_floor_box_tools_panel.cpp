#include "room_floor_box_tools_panel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <utility>

#include "dm_styles.hpp"
#include "draw_utils.hpp"
#include "font_cache.hpp"
#include "utils/string_utils.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "widgets.hpp"

namespace {

constexpr int kPanelMargin = 12;
constexpr int kTopOffset = 56;
constexpr int kPanelWidth = 320;
constexpr int kPanelMinHeight = 460;
constexpr int kPanelMaxHeight = 820;
constexpr int kPanelPadding = 12;
constexpr int kSectionGap = 10;
constexpr int kHeaderHeight = 24;
constexpr int kLineHeight = 18;
constexpr int kChipWidth = 130;
constexpr const char* kBoundaryTag = "boundary";

std::vector<std::string> sorted_set_values(const std::set<std::string>& values) {
    return std::vector<std::string>(values.begin(), values.end());
}

}  // namespace

RoomFloorBoxToolsPanel::RoomFloorBoxToolsPanel() {
    add_button_ = std::make_unique<DMButton>("Create Box", &DMStyles::CreateButton(), 180, DMButton::height());
    delete_button_ = std::make_unique<DMButton>("Delete", &DMStyles::DeleteButton(), 120, DMButton::height());
    name_textbox_ = std::make_unique<DMTextBox>("Name", "");
    enabled_checkbox_ = std::make_unique<DMCheckbox>("Enabled", true);
    position_x_textbox_ = std::make_unique<DMTextBox>("Position X", "0");
    position_z_textbox_ = std::make_unique<DMTextBox>("Position Z", "0");
    width_textbox_ = std::make_unique<DMTextBox>("Width", "0");
    depth_textbox_ = std::make_unique<DMTextBox>("Depth", "0");
    tag_search_textbox_ = std::make_unique<DMTextBox>("Find Tag", "");
    system_enabled_checkbox_ = std::make_unique<DMCheckbox>("Floor Boxes Enabled", false);
    set_recommendation_pool({kBoundaryTag});
}

RoomFloorBoxToolsPanel::~RoomFloorBoxToolsPanel() = default;

void RoomFloorBoxToolsPanel::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    if (!visible_) {
        scroll_offset_ = 0;
    }
    layout_dirty_ = true;
}

void RoomFloorBoxToolsPanel::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void RoomFloorBoxToolsPanel::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_bounds_override_ = bounds;
    panel_bounds_override_active_ = bounds.w > 0 && bounds.h > 0;
    layout_dirty_ = true;
}

void RoomFloorBoxToolsPanel::clear_panel_bounds_override() {
    panel_bounds_override_active_ = false;
    panel_bounds_override_ = SDL_Rect{0, 0, 0, 0};
    layout_dirty_ = true;
}

void RoomFloorBoxToolsPanel::set_system_enabled(bool enabled) {
    if (system_enabled_checkbox_) {
        system_enabled_checkbox_->set_value(enabled);
    }
}

bool RoomFloorBoxToolsPanel::system_enabled() const {
    return system_enabled_checkbox_ ? system_enabled_checkbox_->value() : false;
}

void RoomFloorBoxToolsPanel::set_floor_box_names(const std::vector<std::string>& names) {
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

void RoomFloorBoxToolsPanel::set_selection(int box_index) {
    if (selected_box_index_ == box_index) {
        return;
    }
    selected_box_index_ = box_index;
    layout_dirty_ = true;
}

void RoomFloorBoxToolsPanel::clear_selection() {
    set_selection(-1);
}

void RoomFloorBoxToolsPanel::set_detail_values(const DetailValues& values) {
    if (name_textbox_ && !name_textbox_->is_editing()) {
        name_textbox_->set_value(values.name);
    }
    if (enabled_checkbox_) {
        enabled_checkbox_->set_value(values.enabled);
    }
    if (position_x_textbox_ && !position_x_textbox_->is_editing()) {
        position_x_textbox_->set_value(format_float(values.position_x));
    }
    if (position_z_textbox_ && !position_z_textbox_->is_editing()) {
        position_z_textbox_->set_value(format_float(values.position_z));
    }
    if (width_textbox_ && !width_textbox_->is_editing()) {
        width_textbox_->set_value(format_float(values.width));
    }
    if (depth_textbox_ && !depth_textbox_->is_editing()) {
        depth_textbox_->set_value(format_float(values.depth));
    }
    set_tag_values(values.tags);
}

void RoomFloorBoxToolsPanel::set_recommendation_pool(const std::vector<std::string>& tags) {
    std::set<std::string> deduped;
    for (const auto& raw : tags) {
        const std::string normalized = normalize_tag(raw);
        if (!normalized.empty()) {
            deduped.insert(normalized);
        }
    }
    deduped.insert(std::string(kBoundaryTag));
    const std::vector<std::string> normalized_pool(deduped.begin(), deduped.end());
    if (recommendation_pool_ == normalized_pool) {
        return;
    }
    recommendation_pool_ = normalized_pool;
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
}

void RoomFloorBoxToolsPanel::set_on_select(SelectCallback callback) {
    on_select_ = std::move(callback);
}

void RoomFloorBoxToolsPanel::set_on_add(AddCallback callback) {
    on_add_ = std::move(callback);
}

void RoomFloorBoxToolsPanel::set_on_delete(DeleteCallback callback) {
    on_delete_ = std::move(callback);
}

void RoomFloorBoxToolsPanel::set_on_apply(ApplyCallback callback) {
    on_apply_ = std::move(callback);
}

void RoomFloorBoxToolsPanel::set_on_system_enabled_toggle(SystemEnabledToggleCallback callback) {
    on_system_enabled_toggle_ = std::move(callback);
}

RoomFloorBoxToolsPanel::DetailValues RoomFloorBoxToolsPanel::collect_detail_values() const {
    DetailValues values;
    values.name = name_textbox_ ? name_textbox_->value() : std::string{};
    values.enabled = enabled_checkbox_ ? enabled_checkbox_->value() : true;
    values.position_x = parse_float_or(position_x_textbox_ ? position_x_textbox_->value() : std::string{}, 0.0f);
    values.position_z = parse_float_or(position_z_textbox_ ? position_z_textbox_->value() : std::string{}, 0.0f);
    values.width = parse_float_or(width_textbox_ ? width_textbox_->value() : std::string{}, 0.0f);
    values.depth = parse_float_or(depth_textbox_ ? depth_textbox_->value() : std::string{}, 0.0f);
    values.tags = sorted_set_values(active_tags_);
    return values;
}

bool RoomFloorBoxToolsPanel::handle_event(const SDL_Event& event) {
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

    if (wheel_event) {
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

    bool details_changed = false;
    if (has_selected_box && enabled_checkbox_) {
        const bool before = enabled_checkbox_->value();
        if (enabled_checkbox_->handle_event(event)) {
            handled = true;
            if (before != enabled_checkbox_->value()) {
                details_changed = true;
            }
        }
    }

    auto handle_textbox = [&](DMTextBox* textbox, bool emits_layout) {
        if (!has_selected_box || !textbox) {
            return;
        }
        if (textbox->handle_event(event)) {
            handled = true;
            details_changed = true;
            if (emits_layout) {
                const std::string value = textbox->value();
                if (value != search_input_) {
                    search_input_ = value;
                    search_query_ = to_lower_copy(search_input_);
                    refresh_recommendations();
                    rebuild_tag_buttons();
                    layout_dirty_ = true;
                }
            }
        }
    };
    handle_textbox(name_textbox_.get(), false);
    handle_textbox(position_x_textbox_.get(), false);
    handle_textbox(position_z_textbox_.get(), false);
    handle_textbox(width_textbox_.get(), false);
    handle_textbox(depth_textbox_.get(), false);
    handle_textbox(tag_search_textbox_.get(), true);

    if (has_selected_box) {
        std::optional<std::string> add_tag_value;
        std::optional<std::string> remove_tag_value;
        for (auto& chip : recommended_tag_chips_) {
            if (!chip.button) {
                continue;
            }
            if (chip.button->handle_event(event)) {
                handled = true;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
                    add_tag_value = chip.value;
                }
            }
        }
        for (auto& chip : active_tag_chips_) {
            if (!chip.button) {
                continue;
            }
            if (chip.button->handle_event(event)) {
                handled = true;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
                    remove_tag_value = chip.value;
                }
            }
        }
        if (add_tag_value.has_value()) {
            add_tag(*add_tag_value);
            details_changed = true;
        }
        if (remove_tag_value.has_value()) {
            remove_tag(*remove_tag_value);
            details_changed = true;
        }
    }

    if (details_changed && on_apply_) {
        on_apply_(collect_detail_values());
        handled = true;
    }

    if (pointer_event && pointer_inside_panel) {
        handled = true;
    }

    return handled;
}

void RoomFloorBoxToolsPanel::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) {
        return;
    }
    const_cast<RoomFloorBoxToolsPanel*>(this)->update_layout();

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
    DMFontCache::instance().draw_text(renderer, label_style, "Floor Box Editor", header_rect_.x, header_rect_.y);
    if (system_enabled_checkbox_) {
        system_enabled_checkbox_->render(renderer);
    }
    if (!system_enabled()) {
        return;
    }

    const std::string subtitle = selected_box_index_ >= 0 && selected_box_index_ < static_cast<int>(box_names_.size())
                                     ? box_names_[static_cast<std::size_t>(selected_box_index_)]
                                     : "No floor box selected";
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

    const bool has_selected_box = selected_box_index_ >= 0 &&
                                  selected_box_index_ < static_cast<int>(box_names_.size());
    if (has_selected_box) {
        DMFontCache::instance().draw_text(renderer, label_style, "Floor Box Properties", detail_title_rect_.x, detail_title_rect_.y);
        if (name_textbox_) {
            name_textbox_->render(renderer);
        }
        if (enabled_checkbox_) {
            enabled_checkbox_->render(renderer);
        }
        if (position_x_textbox_) {
            position_x_textbox_->render(renderer);
        }
        if (position_z_textbox_) {
            position_z_textbox_->render(renderer);
        }
        if (width_textbox_) {
            width_textbox_->render(renderer);
        }
        if (depth_textbox_) {
            depth_textbox_->render(renderer);
        }
        if (tag_search_textbox_) {
            tag_search_textbox_->render(renderer);
        }
        DMFontCache::instance().draw_text(renderer, label_style, "Active Tags", tags_label_rect_.x, tags_label_rect_.y);
        DMFontCache::instance().draw_text(renderer, label_style, "Recommended Tags", recommended_label_rect_.x, recommended_label_rect_.y);
        for (const auto& chip : active_tag_chips_) {
            if (chip.button) {
                chip.button->render(renderer);
            }
        }
        for (const auto& chip : recommended_tag_chips_) {
            if (chip.button) {
                chip.button->render(renderer);
            }
        }
    }
}

bool RoomFloorBoxToolsPanel::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    update_layout();
    return point_in_rect(x, y, panel_rect_);
}

void RoomFloorBoxToolsPanel::update_layout() const {
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
        tags_label_rect_ = SDL_Rect{0, 0, 0, 0};
        recommended_label_rect_ = SDL_Rect{0, 0, 0, 0};
        if (name_textbox_) name_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (enabled_checkbox_) enabled_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (position_x_textbox_) position_x_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (position_z_textbox_) position_z_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (width_textbox_) width_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (depth_textbox_) depth_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (tag_search_textbox_) tag_search_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (add_button_) add_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (delete_button_) delete_button_->set_rect(SDL_Rect{0, 0, 0, 0});
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

    const int text_h = name_textbox_ ? name_textbox_->preferred_height(controls_w) : DMTextBox::height();
    const bool has_selected_box = selected_box_index_ >= 0 &&
                                  selected_box_index_ < static_cast<int>(box_names_.size());

    int controls_height = 0;
    controls_height += DMButton::height();
    controls_height += kSectionGap;
    controls_height += DMButton::height();
    if (has_selected_box) {
        controls_height += kSectionGap + kLineHeight + row_gap;
        controls_height += text_h + row_gap; // name
        controls_height += DMCheckbox::height() + row_gap; // enabled
        controls_height += text_h + row_gap; // x
        controls_height += text_h + row_gap; // z
        controls_height += text_h + row_gap; // width
        controls_height += text_h + row_gap; // depth
        controls_height += text_h + row_gap; // tag search
        controls_height += kLineHeight + row_gap; // active label
        controls_height += layout_tag_chips(const_cast<std::vector<TagChip>&>(active_tag_chips_), controls_x, controls_w, 0, false);
        controls_height += kSectionGap;
        controls_height += kLineHeight + row_gap; // recommended label
        controls_height += layout_tag_chips(const_cast<std::vector<TagChip>&>(recommended_tag_chips_), controls_x, controls_w, 0, false);
    }

    const int list_top = subtitle_rect_.y + 22;
    const int available_list_height =
        (panel_rect_.y + panel_rect_.h) - kPanelPadding - controls_height - kSectionGap - list_top;
    list_clip_rect_ = SDL_Rect{controls_x, list_top, controls_w, std::max(0, available_list_height)};

    int y = list_clip_rect_.y + list_clip_rect_.h + kSectionGap;
    if (add_button_) add_button_->set_rect(SDL_Rect{controls_x, y, controls_w, DMButton::height()});
    y += DMButton::height() + kSectionGap;
    if (delete_button_) delete_button_->set_rect(SDL_Rect{controls_x, y, controls_w, DMButton::height()});
    y += DMButton::height() + kSectionGap;

    if (has_selected_box) {
        detail_title_rect_ = SDL_Rect{controls_x, y, controls_w, kLineHeight};
        y += kLineHeight + row_gap;
        if (name_textbox_) name_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, text_h});
        y += text_h + row_gap;
        if (enabled_checkbox_) enabled_checkbox_->set_rect(SDL_Rect{controls_x, y, controls_w, DMCheckbox::height()});
        y += DMCheckbox::height() + row_gap;
        if (position_x_textbox_) position_x_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, text_h});
        y += text_h + row_gap;
        if (position_z_textbox_) position_z_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, text_h});
        y += text_h + row_gap;
        if (width_textbox_) width_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, text_h});
        y += text_h + row_gap;
        if (depth_textbox_) depth_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, text_h});
        y += text_h + row_gap;
        if (tag_search_textbox_) tag_search_textbox_->set_rect(SDL_Rect{controls_x, y, controls_w, text_h});
        y += text_h + row_gap;

        tags_label_rect_ = SDL_Rect{controls_x, y, controls_w, kLineHeight};
        y += kLineHeight + row_gap;
        y = layout_tag_chips(const_cast<std::vector<TagChip>&>(active_tag_chips_), controls_x, controls_w, y, true);
        y += kSectionGap;

        recommended_label_rect_ = SDL_Rect{controls_x, y, controls_w, kLineHeight};
        y += kLineHeight + row_gap;
        layout_tag_chips(const_cast<std::vector<TagChip>&>(recommended_tag_chips_), controls_x, controls_w, y, true);
    } else {
        detail_title_rect_ = SDL_Rect{0, 0, 0, 0};
        tags_label_rect_ = SDL_Rect{0, 0, 0, 0};
        recommended_label_rect_ = SDL_Rect{0, 0, 0, 0};
        if (name_textbox_) name_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (enabled_checkbox_) enabled_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (position_x_textbox_) position_x_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (position_z_textbox_) position_z_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (width_textbox_) width_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (depth_textbox_) depth_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (tag_search_textbox_) tag_search_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
    }

    layout_box_buttons();
    layout_dirty_ = false;
}

void RoomFloorBoxToolsPanel::layout_box_buttons() const {
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

int RoomFloorBoxToolsPanel::layout_tag_chips(std::vector<TagChip>& chips,
                                              int content_x,
                                              int content_w,
                                              int start_y,
                                              bool apply) const {
    if (chips.empty()) {
        return start_y + DMButton::height();
    }

    const int gap = DMSpacing::small_gap();
    const int available = std::max(1, content_w);
    const int columns = std::max(1, (available + gap) / (kChipWidth + gap));

    int row = 0;
    int col = 0;
    for (auto& chip : chips) {
        const int x = content_x + col * (kChipWidth + gap);
        const int y = start_y + row * (DMButton::height() + gap);
        if (apply && chip.button) {
            chip.button->set_rect(SDL_Rect{x, y, kChipWidth, DMButton::height()});
        }
        ++col;
        if (col >= columns) {
            col = 0;
            ++row;
        }
    }

    const int rows = row + (col > 0 ? 1 : 0);
    return start_y + rows * DMButton::height() + std::max(0, rows - 1) * gap;
}

void RoomFloorBoxToolsPanel::scroll_by(int delta) {
    if (max_scroll_ <= 0) {
        scroll_offset_ = 0;
        return;
    }
    scroll_offset_ = std::clamp(scroll_offset_ + delta, 0, max_scroll_);
    layout_box_buttons();
}

std::string RoomFloorBoxToolsPanel::normalize_tag(std::string_view raw) {
    const std::string lowered = to_lower_copy(vibble::strings::trim_copy(std::string(raw)));
    if (lowered.empty()) {
        return {};
    }
    return lowered;
}

std::string RoomFloorBoxToolsPanel::to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool RoomFloorBoxToolsPanel::starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

void RoomFloorBoxToolsPanel::set_tag_values(const std::vector<std::string>& tags) {
    std::set<std::string> normalized;
    for (const auto& raw : tags) {
        const std::string token = normalize_tag(raw);
        if (!token.empty()) {
            normalized.insert(token);
        }
    }
    if (normalized == active_tags_) {
        return;
    }
    active_tags_ = std::move(normalized);
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
}

void RoomFloorBoxToolsPanel::refresh_recommendations() {
    std::vector<std::string> exact;
    std::vector<std::string> prefix;
    std::vector<std::string> contains;
    std::set<std::string> seen;

    const std::string normalized_query = normalize_tag(search_query_);
    for (const auto& tag : recommendation_pool_) {
        if (active_tags_.count(tag) != 0 || seen.count(tag) != 0) {
            continue;
        }
        if (normalized_query.empty()) {
            contains.push_back(tag);
            seen.insert(tag);
            continue;
        }
        if (tag == normalized_query) {
            exact.push_back(tag);
            seen.insert(tag);
            continue;
        }
        if (starts_with(tag, normalized_query)) {
            prefix.push_back(tag);
            seen.insert(tag);
            continue;
        }
        if (tag.find(normalized_query) != std::string::npos) {
            contains.push_back(tag);
            seen.insert(tag);
        }
    }

    std::sort(exact.begin(), exact.end());
    std::sort(prefix.begin(), prefix.end());
    std::sort(contains.begin(), contains.end());

    recommended_tags_.clear();
    recommended_tags_.insert(recommended_tags_.end(), exact.begin(), exact.end());
    recommended_tags_.insert(recommended_tags_.end(), prefix.begin(), prefix.end());
    recommended_tags_.insert(recommended_tags_.end(), contains.begin(), contains.end());

    if (!normalized_query.empty() &&
        active_tags_.count(normalized_query) == 0 &&
        std::find(recommended_tags_.begin(), recommended_tags_.end(), normalized_query) == recommended_tags_.end()) {
        recommended_tags_.insert(recommended_tags_.begin(), normalized_query);
    }
}

void RoomFloorBoxToolsPanel::rebuild_tag_buttons() {
    active_tag_chips_.clear();
    recommended_tag_chips_.clear();

    for (const auto& value : active_tags_) {
        TagChip chip;
        chip.value = value;
        chip.button = std::make_unique<DMButton>(value, &DMStyles::HeaderButton(), kChipWidth, DMButton::height());
        active_tag_chips_.push_back(std::move(chip));
    }
    for (const auto& value : recommended_tags_) {
        TagChip chip;
        chip.value = value;
        chip.button = std::make_unique<DMButton>(value, &DMStyles::AccentButton(), kChipWidth, DMButton::height());
        recommended_tag_chips_.push_back(std::move(chip));
    }
}

void RoomFloorBoxToolsPanel::add_tag(const std::string& tag) {
    const std::string normalized = normalize_tag(tag);
    if (normalized.empty()) {
        return;
    }
    if (!active_tags_.insert(normalized).second) {
        return;
    }
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
}

void RoomFloorBoxToolsPanel::remove_tag(const std::string& tag) {
    const std::string normalized = normalize_tag(tag);
    if (normalized.empty() || active_tags_.erase(normalized) == 0) {
        return;
    }
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
}

bool RoomFloorBoxToolsPanel::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point point{x, y};
    return SDL_PointInRect(&point, &rect);
}

float RoomFloorBoxToolsPanel::parse_float_or(const std::string& text, float fallback) {
    try {
        return std::stof(text);
    } catch (...) {
        return fallback;
    }
}

std::string RoomFloorBoxToolsPanel::format_float(float value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    const float rounded = std::round(value * 100.0f) / 100.0f;
    std::string text = std::to_string(rounded);
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    if (text.empty() || text == "-0") {
        text = "0";
    }
    return text;
}
