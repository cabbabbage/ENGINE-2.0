#include "room_anchor_tools_panel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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
constexpr int kLineHeight = 18;

int parse_int_or(const std::string& text, int fallback) {
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

bool parse_bool_or(const std::string& text, bool fallback) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    return fallback;
}

}  // namespace

RoomAnchorToolsPanel::RoomAnchorToolsPanel() {
    add_button_ = std::make_unique<DMButton>("Add Anchor", &DMStyles::CreateButton(), 180, DMButton::height());
    rename_textbox_ = std::make_unique<DMTextBox>("Rename", "");
    depth_textbox_ = std::make_unique<DMTextBox>("Depth Offset", "0");
    flip_horizontal_textbox_ = std::make_unique<DMTextBox>("Flip Horizontal (1/0)", "1");
    flip_vertical_textbox_ = std::make_unique<DMTextBox>("Flip Vertical (1/0)", "1");
    rotation_slider_ = std::make_unique<DMSlider>("Rotation Degrees", -360, 360, 0);
    hidden_checkbox_ = std::make_unique<DMCheckbox>("Hidden", false);
    resolve_x_checkbox_ = std::make_unique<DMCheckbox>("Resolve X", true);
    onion_skin_checkbox_ = std::make_unique<DMCheckbox>("Show onion skin (prev/next)", false);
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

void RoomAnchorToolsPanel::set_detail_values(const DetailValues& values) {
    if (depth_textbox_ && !depth_textbox_->is_editing()) {
        depth_textbox_->set_value(std::to_string(values.depth_offset));
    }
    if (flip_horizontal_textbox_ && !flip_horizontal_textbox_->is_editing()) {
        flip_horizontal_textbox_->set_value(values.flip_horizontal ? "1" : "0");
    }
    if (flip_vertical_textbox_ && !flip_vertical_textbox_->is_editing()) {
        flip_vertical_textbox_->set_value(values.flip_vertical ? "1" : "0");
    }
    if (rotation_slider_) {
        int rotation_degrees = static_cast<int>(std::lround(values.rotation_degrees));
        rotation_slider_->set_value(rotation_degrees);
    }
    if (hidden_checkbox_) {
        hidden_checkbox_->set_value(values.hidden);
    }
    if (resolve_x_checkbox_) {
        resolve_x_checkbox_->set_value(values.resolve_x);
    }
}

void RoomAnchorToolsPanel::set_onion_skin_enabled(bool enabled) {
    if (onion_skin_checkbox_) {
        onion_skin_checkbox_->set_value(enabled);
    }
}

bool RoomAnchorToolsPanel::onion_skin_enabled() const {
    return onion_skin_checkbox_ ? onion_skin_checkbox_->value() : false;
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

void RoomAnchorToolsPanel::set_on_apply_details(ApplyDetailsCallback callback) {
    on_apply_details_ = std::move(callback);
}

void RoomAnchorToolsPanel::set_on_propagate(PropagateCallback callback) {
    on_propagate_ = std::move(callback);
}

void RoomAnchorToolsPanel::set_on_onion_skin_toggle(OnionSkinToggleCallback callback) {
    on_onion_skin_toggle_ = std::move(callback);
}

void RoomAnchorToolsPanel::set_on_open_candidates(OpenCandidatesCallback callback) {
    on_open_candidates_ = std::move(callback);
}

RoomAnchorToolsPanel::DetailValues RoomAnchorToolsPanel::collect_detail_values() const {
    DetailValues values;
    values.depth_offset = parse_int_or(depth_textbox_ ? depth_textbox_->value() : std::string{}, 0);
    values.flip_horizontal = parse_bool_or(flip_horizontal_textbox_ ? flip_horizontal_textbox_->value() : std::string{}, true);
    values.flip_vertical = parse_bool_or(flip_vertical_textbox_ ? flip_vertical_textbox_->value() : std::string{}, true);
    values.rotation_degrees = rotation_slider_ ? static_cast<float>(rotation_slider_->value()) : 0.0f;
    values.hidden = hidden_checkbox_ ? hidden_checkbox_->value() : false;
    values.resolve_x = resolve_x_checkbox_ ? resolve_x_checkbox_->value() : true;
    if (!std::isfinite(values.rotation_degrees)) {
        values.rotation_degrees = 0.0f;
    }
    return values;
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
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_RIGHT &&
            i < anchor_names_.size() &&
            point_in_rect(pointer_x, pointer_y, list_clip_rect_) &&
            point_in_rect(pointer_x, pointer_y, row_rect)) {
            selected_anchor_name_ = anchor_names_[i];
            if (!rename_textbox_->is_editing()) {
                rename_textbox_->set_value(selected_anchor_name_);
            }
            if (on_select_) {
                on_select_(selected_anchor_name_);
            }
            if (on_open_candidates_) {
                on_open_candidates_(selected_anchor_name_, SDL_Point{pointer_x, pointer_y}, row_rect);
            }
            return true;
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

    bool details_changed = false;
    if (has_selected_anchor && depth_textbox_ && depth_textbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_anchor && flip_horizontal_textbox_ && flip_horizontal_textbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_anchor && flip_vertical_textbox_ && flip_vertical_textbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_anchor && rotation_slider_ && rotation_slider_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_anchor && hidden_checkbox_ && hidden_checkbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_anchor && resolve_x_checkbox_ && resolve_x_checkbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (details_changed && on_apply_details_) {
        on_apply_details_(collect_detail_values());
        handled = true;
    }

    const bool detail_editing =
        (depth_textbox_ && depth_textbox_->is_editing()) ||
        (flip_horizontal_textbox_ && flip_horizontal_textbox_->is_editing()) ||
        (flip_vertical_textbox_ && flip_vertical_textbox_->is_editing());
    if ((event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN) &&
        ((rename_textbox_ && rename_textbox_->is_editing()) || detail_editing)) {
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
    if (onion_skin_checkbox_) {
        onion_skin_checkbox_->render(renderer);
    }
    const bool has_selected_anchor = !selected_anchor_name_.empty();
    if (has_selected_anchor && rename_textbox_) {
        rename_textbox_->render(renderer);
    }
    if (has_selected_anchor) {
        DMFontCache::instance().draw_text(renderer, label_style, "Anchor Properties", detail_title_rect_.x, detail_title_rect_.y);
        if (depth_textbox_) {
            depth_textbox_->render(renderer);
        }
        if (flip_horizontal_textbox_) {
            flip_horizontal_textbox_->render(renderer);
        }
        if (flip_vertical_textbox_) {
            flip_vertical_textbox_->render(renderer);
        }
        if (rotation_slider_) {
            rotation_slider_->render(renderer);
        }
        if (hidden_checkbox_) {
            hidden_checkbox_->render(renderer);
        }
        if (resolve_x_checkbox_) {
            resolve_x_checkbox_->render(renderer);
        }
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
    const int depth_h = depth_textbox_ ? depth_textbox_->preferred_height(controls_width) : DMTextBox::height();
    const int flip_h_h = flip_horizontal_textbox_ ? flip_horizontal_textbox_->preferred_height(controls_width) : DMTextBox::height();
    const int flip_v_h = flip_vertical_textbox_ ? flip_vertical_textbox_->preferred_height(controls_width) : DMTextBox::height();
    const int rotation_h = rotation_slider_ ? rotation_slider_->preferred_height(controls_width) : DMSlider::height();
    const int hidden_h = hidden_checkbox_ ? DMCheckbox::height() : 0;
    const int resolve_x_h = resolve_x_checkbox_ ? DMCheckbox::height() : 0;
    int controls_height = 0;
    controls_height += DMButton::height();                          // add
    controls_height += kSectionGap;
    controls_height += DMCheckbox::height();                        // onion skin
    controls_height += kSectionGap;
    if (has_selected_anchor) {
        controls_height += rename_h;                                // rename text
        controls_height += kSectionGap;
        controls_height += kLineHeight;                             // detail title
        controls_height += row_gap;
        controls_height += depth_h;
        controls_height += row_gap;
        controls_height += flip_h_h;
        controls_height += row_gap;
        controls_height += flip_v_h;
        controls_height += row_gap;
        controls_height += rotation_h;
        controls_height += row_gap;
        controls_height += hidden_h;
        controls_height += row_gap;
        controls_height += resolve_x_h;
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
    if (onion_skin_checkbox_) {
        onion_skin_checkbox_->set_rect(SDL_Rect{controls_x, row_y, controls_width, DMCheckbox::height()});
        row_y += DMCheckbox::height() + kSectionGap;
    }
    if (rename_textbox_) {
        if (has_selected_anchor) {
            rename_textbox_->set_rect(SDL_Rect{controls_x, row_y, controls_width, rename_h});
            row_y += rename_h + kSectionGap;
        } else {
            rename_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
    }

    if (has_selected_anchor) {
        detail_title_rect_ = SDL_Rect{controls_x, row_y, controls_width, kLineHeight};
        row_y += kLineHeight + row_gap;
        if (depth_textbox_) {
            depth_textbox_->set_rect(SDL_Rect{controls_x, row_y, controls_width, depth_h});
            row_y += depth_h + row_gap;
        }
        if (flip_horizontal_textbox_) {
            flip_horizontal_textbox_->set_rect(SDL_Rect{controls_x, row_y, controls_width, flip_h_h});
            row_y += flip_h_h + row_gap;
        }
        if (flip_vertical_textbox_) {
            flip_vertical_textbox_->set_rect(SDL_Rect{controls_x, row_y, controls_width, flip_v_h});
            row_y += flip_v_h + row_gap;
        }
        if (rotation_slider_) {
            rotation_slider_->set_rect(SDL_Rect{controls_x, row_y, controls_width, rotation_h});
            row_y += rotation_h + row_gap;
        }
        if (hidden_checkbox_) {
            hidden_checkbox_->set_rect(SDL_Rect{controls_x, row_y, controls_width, hidden_h});
            row_y += hidden_h + row_gap;
        }
        if (resolve_x_checkbox_) {
            resolve_x_checkbox_->set_rect(SDL_Rect{controls_x, row_y, controls_width, resolve_x_h});
            row_y += resolve_x_h + kSectionGap;
        }
    } else {
        detail_title_rect_ = SDL_Rect{0, 0, 0, 0};
        if (depth_textbox_) {
            depth_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (flip_horizontal_textbox_) {
            flip_horizontal_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (flip_vertical_textbox_) {
            flip_vertical_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (rotation_slider_) {
            rotation_slider_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (hidden_checkbox_) {
            hidden_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        if (resolve_x_checkbox_) {
            resolve_x_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
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
