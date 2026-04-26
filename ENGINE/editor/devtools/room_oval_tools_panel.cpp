#include "room_oval_tools_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
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
constexpr int kPanelWidth = 340;
constexpr int kPanelMinHeight = 380;
constexpr int kPanelMaxHeight = 900;
constexpr int kPanelPadding = 10;
constexpr int kSectionGap = 8;
constexpr int kRowGap = 4;
constexpr int kHeaderHeight = 24;
constexpr int kLineHeight = 18;
constexpr int kAdvancedCardPadding = 6;
constexpr int kListMinHeight = 56;
constexpr int kListMaxHeight = 180;

const std::vector<std::string>& scaling_method_options() {
    static const std::vector<std::string> options{
        "Parent",
        "Real 3D Point",
        "Relative 2D Anchor Point",
        "Real 3D Floor Point",
    };
    return options;
}

}  // namespace

RoomOvalToolsPanel::RoomOvalToolsPanel() {
    add_oval_button_ = std::make_unique<DMButton>("Add Oval", &DMStyles::CreateButton(), 180, DMButton::height());
    oval_name_textbox_ = std::make_unique<DMTextBox>("Name", "");
    width_textbox_ = std::make_unique<DMTextBox>("Width Radius X", "48");
    height_textbox_ = std::make_unique<DMTextBox>("Height Radius Z", "24");
    radius_offset_slider_ = std::make_unique<DMSlider>("Radius Offset (deg)", 0, 360, 0);
    apply_oval_properties_button_ = std::make_unique<DMButton>("Apply Oval Properties", &DMStyles::PrimaryButton(), 180, DMButton::height());
    delete_oval_button_ = std::make_unique<DMButton>("Delete Oval", &DMStyles::DeleteButton(), 180, DMButton::height());

    center_depth_textbox_ = std::make_unique<DMTextBox>("Depth Offset", "0");
    center_rotation_slider_ = std::make_unique<DMSlider>("Rotation Degrees", -360, 360, 0);
    center_hidden_checkbox_ = std::make_unique<DMCheckbox>("Hidden", false);
    center_advanced_options_button_ = std::make_unique<DMButton>("Advanced Options (Show)", &DMStyles::ListButton(), 180, DMButton::height());
    center_flip_horizontal_checkbox_ = std::make_unique<DMCheckbox>("Flip Horizontal", true);
    center_flip_vertical_checkbox_ = std::make_unique<DMCheckbox>("Flip Vertical", true);
    center_resolve_x_checkbox_ = std::make_unique<DMCheckbox>("Resolve X", true);
    center_scaling_method_dropdown_ = std::make_unique<DMDropdown>("Scaling Method", scaling_method_options(), 0);
    center_apply_next_frame_button_ = std::make_unique<DMButton>("Copy To Next Frame", &DMStyles::PrimaryButton(), 170, DMButton::height());
    center_apply_animation_button_ = std::make_unique<DMButton>("Copy To Animation", &DMStyles::PrimaryButton(), 170, DMButton::height());
    center_apply_asset_button_ = std::make_unique<DMButton>("Copy To Asset", &DMStyles::PrimaryButton(), 170, DMButton::height());

    point_count_stepper_ = std::make_unique<DMNumericStepper>("Point Count", 0, 4096, 0);
    point_count_stepper_->set_step(1);
    point_count_stepper_->set_on_change([this](int requested_value) {
        if (requested_value > point_count_) {
            if (on_increment_point_count_) {
                on_increment_point_count_();
            }
        } else if (requested_value < point_count_) {
            if (on_decrement_point_count_) {
                on_decrement_point_count_();
            }
        }
        if (point_count_stepper_) {
            point_count_stepper_->set_value(point_count_);
        }
    });

    point_rotation_slider_ = std::make_unique<DMSlider>("Rotation Degrees", -360, 360, 0);
    point_hidden_checkbox_ = std::make_unique<DMCheckbox>("Hidden", false);
    advanced_options_button_ = std::make_unique<DMButton>("Advanced Options (Show)", &DMStyles::ListButton(), 180, DMButton::height());
    point_resolve_x_checkbox_ = std::make_unique<DMCheckbox>("Resolve X", true);
    point_scaling_method_dropdown_ = std::make_unique<DMDropdown>("Scaling Method", scaling_method_options(), 0);
}

RoomOvalToolsPanel::~RoomOvalToolsPanel() = default;

void RoomOvalToolsPanel::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_bounds_override_ = bounds;
    panel_bounds_override_active_ = bounds.w > 0 && bounds.h > 0;
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::clear_panel_bounds_override() {
    panel_bounds_override_active_ = false;
    panel_bounds_override_ = SDL_Rect{0, 0, 0, 0};
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_oval_names(const std::vector<std::string>& names) {
    if (oval_names_ == names) {
        return;
    }
    oval_names_ = names;
    oval_buttons_.clear();
    oval_buttons_.reserve(oval_names_.size());
    for (const std::string& name : oval_names_) {
        oval_buttons_.push_back(std::make_unique<DMButton>(name, &DMStyles::ListButton(), 220, DMButton::height()));
    }
    if (selected_oval_index_ >= static_cast<int>(oval_names_.size())) {
        selected_oval_index_ = oval_names_.empty() ? -1 : static_cast<int>(oval_names_.size()) - 1;
    }
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_selected_oval_index(int index) {
    const int clamped_index =
        (index >= 0 && index < static_cast<int>(oval_names_.size())) ? index : -1;
    if (selected_oval_index_ == clamped_index) {
        return;
    }
    selected_oval_index_ = clamped_index;
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_oval_properties(const OvalProperties& properties) {
    oval_properties_ = properties;
    if (oval_name_textbox_ && !oval_name_textbox_->is_editing()) {
        oval_name_textbox_->set_value(properties.name);
    }
    if (width_textbox_ && !width_textbox_->is_editing()) {
        width_textbox_->set_value(format_float(properties.width_radius_x));
    }
    if (height_textbox_ && !height_textbox_->is_editing()) {
        height_textbox_->set_value(format_float(properties.height_radius_z));
    }
    if (radius_offset_slider_) {
        const int slider_value = static_cast<int>(std::lround(std::clamp(properties.radius_offset_degrees, 0.0f, 360.0f)));
        radius_offset_slider_->set_value(slider_value);
    }
}

void RoomOvalToolsPanel::set_asset_binding_status(const AssetBindingStatus& status) {
    if (asset_binding_status_.kind == status.kind &&
        asset_binding_status_.detail == status.detail) {
        return;
    }
    asset_binding_status_ = status;
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_center_anchor_status(const std::string& center_name, bool present) {
    center_anchor_name_ = center_name;
    center_anchor_present_ = present;
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_center_selected(bool selected) {
    if (center_selected_ == selected) {
        return;
    }
    center_selected_ = selected;
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_center_detail_values(const CenterDetailValues& values) {
    if (center_depth_textbox_ && !center_depth_textbox_->is_editing()) {
        center_depth_textbox_->set_value(format_float(values.depth_offset));
    }
    if (center_rotation_slider_) {
        center_rotation_slider_->set_value(static_cast<int>(std::lround(values.rotation_degrees)));
    }
    if (center_hidden_checkbox_) {
        center_hidden_checkbox_->set_value(values.hidden);
    }
    if (center_resolve_x_checkbox_) {
        center_resolve_x_checkbox_->set_value(values.resolve_x);
    }
    if (center_flip_horizontal_checkbox_) {
        center_flip_horizontal_checkbox_->set_value(values.flip_horizontal);
    }
    if (center_flip_vertical_checkbox_) {
        center_flip_vertical_checkbox_->set_value(values.flip_vertical);
    }
    if (center_scaling_method_dropdown_) {
        center_scaling_method_dropdown_->set_selected(scaling_method_to_dropdown_index(values.scaling_method));
    }
}

void RoomOvalToolsPanel::set_point_count(int count) {
    const int clamped = std::max(0, count);
    if (point_count_ == clamped) {
        return;
    }
    point_count_ = clamped;
    if (point_count_stepper_) {
        const int stepper_max = std::max(4096, point_count_);
        point_count_stepper_->set_range(0, stepper_max);
        point_count_stepper_->set_value(point_count_);
    }
    if (selected_point_index_ >= point_count_) {
        selected_point_index_ = point_count_ > 0 ? point_count_ - 1 : -1;
    }
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_selected_point_index(int index) {
    const int clamped_index = (index >= 0 && index < point_count_) ? index : -1;
    if (selected_point_index_ == clamped_index) {
        return;
    }
    selected_point_index_ = clamped_index;
    layout_dirty_ = true;
}

void RoomOvalToolsPanel::set_point_detail_values(const PointDetailValues& values) {
    if (point_rotation_slider_) {
        point_rotation_slider_->set_value(static_cast<int>(std::lround(values.rotation_degrees)));
    }
    if (point_hidden_checkbox_) {
        point_hidden_checkbox_->set_value(values.hidden);
    }
    if (point_resolve_x_checkbox_) {
        point_resolve_x_checkbox_->set_value(values.resolve_x);
    }
    if (point_scaling_method_dropdown_) {
        point_scaling_method_dropdown_->set_selected(scaling_method_to_dropdown_index(values.scaling_method));
    }
}

void RoomOvalToolsPanel::set_on_select_oval(SelectOvalCallback callback) {
    on_select_oval_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_add_oval(AddOvalCallback callback) {
    on_add_oval_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_delete_oval(DeleteOvalCallback callback) {
    on_delete_oval_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_apply_oval_properties(ApplyOvalPropertiesCallback callback) {
    on_apply_oval_properties_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_open_candidates(OpenCandidatesCallback callback) {
    on_open_candidates_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_increment_point_count(IncrementPointCountCallback callback) {
    on_increment_point_count_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_decrement_point_count(DecrementPointCountCallback callback) {
    on_decrement_point_count_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_apply_point_details(ApplyPointDetailsCallback callback) {
    on_apply_point_details_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_apply_center_details(ApplyCenterDetailsCallback callback) {
    on_apply_center_details_ = std::move(callback);
}

void RoomOvalToolsPanel::set_on_propagate_center(PropagateCenterCallback callback) {
    on_propagate_center_ = std::move(callback);
}

bool RoomOvalToolsPanel::handle_event(const SDL_Event& event) {
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

    const bool pointer_inside_panel =
        point_in_rect(pointer_x, pointer_y, panel_rect_) ||
        point_in_rect(pointer_x, pointer_y, center_panel_rect_);
    bool handled = false;
    const bool has_selected_oval = selected_oval_index_ >= 0 &&
                                   selected_oval_index_ < static_cast<int>(oval_names_.size());
    const bool has_selected_center = has_selected_oval && center_selected_ && center_anchor_present_;
    const bool has_selected_point = false;

    layout_oval_buttons();

    for (std::size_t i = 0; i < oval_buttons_.size(); ++i) {
        DMButton* button = oval_buttons_[i].get();
        if (!button) {
            continue;
        }
        const SDL_Rect row_rect = button->rect();
        const bool row_visible = row_rect.y + row_rect.h >= oval_list_clip_rect_.y &&
                                 row_rect.y <= oval_list_clip_rect_.y + oval_list_clip_rect_.h;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_RIGHT &&
            i < oval_names_.size() &&
            row_visible &&
            point_in_rect(pointer_x, pointer_y, oval_list_clip_rect_) &&
            point_in_rect(pointer_x, pointer_y, row_rect)) {
            selected_oval_index_ = static_cast<int>(i);
            layout_dirty_ = true;
            if (on_select_oval_) {
                on_select_oval_(static_cast<int>(i));
            }
            if (on_open_candidates_) {
                devmode::CandidateSourceContext source_context = devmode::CandidateSourceContext::OvalCenter;
                bool valid_source = false;
                if (!center_selected_ && selected_point_index_ >= 0) {
                    source_context = devmode::CandidateSourceContext::OvalPoint;
                    valid_source = true;
                } else if (center_selected_ && selected_point_index_ < 0) {
                    source_context = devmode::CandidateSourceContext::OvalCenter;
                    valid_source = true;
                }
                if (valid_source) {
                    on_open_candidates_(oval_names_[i], SDL_Point{pointer_x, pointer_y}, row_rect, source_context);
                }
            }
            return true;
        }
        if (button->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                event.button.button == SDL_BUTTON_LEFT &&
                on_select_oval_) {
                on_select_oval_(static_cast<int>(i));
            }
        }
    }

    if (add_oval_button_ && add_oval_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_add_oval_) {
            on_add_oval_();
        }
    }

    if (has_selected_oval) {
        bool oval_properties_changed = false;
        if (oval_name_textbox_ && oval_name_textbox_->handle_event(event)) {
            handled = true;
            oval_properties_changed = true;
        }
        if (width_textbox_ && width_textbox_->handle_event(event)) {
            handled = true;
            oval_properties_changed = true;
        }
        if (height_textbox_ && height_textbox_->handle_event(event)) {
            handled = true;
            oval_properties_changed = true;
        }
        if (radius_offset_slider_ && radius_offset_slider_->handle_event(event)) {
            handled = true;
            oval_properties_changed = true;
        }
        if (oval_properties_changed && on_apply_oval_properties_) {
            on_apply_oval_properties_(collect_oval_properties());
        }
        if (apply_oval_properties_button_ && apply_oval_properties_button_->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                event.button.button == SDL_BUTTON_LEFT &&
                on_apply_oval_properties_) {
                on_apply_oval_properties_(collect_oval_properties());
            }
        }

        if (delete_oval_button_ && delete_oval_button_->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                event.button.button == SDL_BUTTON_LEFT &&
                on_delete_oval_) {
                on_delete_oval_();
            }
        }
    }

    if (point_count_stepper_ && point_count_stepper_->handle_event(event)) {
        handled = true;
    }

    bool center_details_changed = false;
    if (has_selected_center && center_depth_textbox_ && center_depth_textbox_->handle_event(event)) {
        handled = true;
        center_details_changed = true;
    }
    if (has_selected_center && center_rotation_slider_ && center_rotation_slider_->handle_event(event)) {
        handled = true;
        center_details_changed = true;
    }
    if (has_selected_center && center_hidden_checkbox_ && center_hidden_checkbox_->handle_event(event)) {
        handled = true;
        center_details_changed = true;
    }
    if (has_selected_center && center_advanced_options_button_ &&
        center_advanced_options_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            center_advanced_options_expanded_ = !center_advanced_options_expanded_;
            layout_dirty_ = true;
        }
    }
    if (has_selected_center && center_advanced_options_expanded_ &&
        center_resolve_x_checkbox_ && center_resolve_x_checkbox_->handle_event(event)) {
        handled = true;
        center_details_changed = true;
    }
    if (has_selected_center && center_advanced_options_expanded_ &&
        center_flip_vertical_checkbox_ && center_flip_vertical_checkbox_->handle_event(event)) {
        handled = true;
        center_details_changed = true;
    }
    if (has_selected_center && center_advanced_options_expanded_ &&
        center_flip_horizontal_checkbox_ && center_flip_horizontal_checkbox_->handle_event(event)) {
        handled = true;
        center_details_changed = true;
    }
    if (has_selected_center && center_advanced_options_expanded_ &&
        center_scaling_method_dropdown_ && center_scaling_method_dropdown_->handle_event(event)) {
        handled = true;
        center_details_changed = true;
    }
    if (center_details_changed && on_apply_center_details_) {
        on_apply_center_details_(collect_center_detail_values());
    }

    if (has_selected_center && center_apply_next_frame_button_ &&
        center_apply_next_frame_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_propagate_center_) {
            on_propagate_center_(PropagationScope::NextFrame);
        }
    }
    if (has_selected_center && center_apply_animation_button_ &&
        center_apply_animation_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_propagate_center_) {
            on_propagate_center_(PropagationScope::Animation);
        }
    }
    if (has_selected_center && center_apply_asset_button_ &&
        center_apply_asset_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT &&
            on_propagate_center_) {
            on_propagate_center_(PropagationScope::Asset);
        }
    }

    bool details_changed = false;
    if (has_selected_point && point_rotation_slider_ && point_rotation_slider_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_point && point_hidden_checkbox_ && point_hidden_checkbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_point && advanced_options_button_ && advanced_options_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            advanced_options_expanded_ = !advanced_options_expanded_;
            layout_dirty_ = true;
        }
    }
    if (has_selected_point && advanced_options_expanded_ &&
        point_resolve_x_checkbox_ && point_resolve_x_checkbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (has_selected_point && advanced_options_expanded_ &&
        point_scaling_method_dropdown_ && point_scaling_method_dropdown_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (details_changed && on_apply_point_details_) {
        on_apply_point_details_(collect_point_detail_values());
    }

    const bool text_editing =
        (oval_name_textbox_ && oval_name_textbox_->is_editing()) ||
        (width_textbox_ && width_textbox_->is_editing()) ||
        (height_textbox_ && height_textbox_->is_editing()) ||
        (center_depth_textbox_ && center_depth_textbox_->is_editing());
    if ((event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN) && text_editing) {
        handled = true;
    }

    if (pointer_event && pointer_inside_panel) {
        handled = true;
    }
    return handled;
}

void RoomOvalToolsPanel::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) {
        return;
    }
    const_cast<RoomOvalToolsPanel*>(this)->update_layout();
    const bool has_selected_oval = selected_oval_index_ >= 0 &&
                                   selected_oval_index_ < static_cast<int>(oval_names_.size());
    const bool has_selected_center = has_selected_oval && center_selected_ && center_anchor_present_;
    const bool has_selected_point = false;

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
    DMFontCache::instance().draw_text(renderer, label_style, "Oval Anchor Tools", header_rect_.x, header_rect_.y);

    SDL_SetRenderDrawColor(renderer, 20, 24, 30, 180);
    sdl_render::FillRect(renderer, &oval_list_clip_rect_);
    dm_draw::DrawRoundedOutline(renderer, oval_list_clip_rect_, 4, 1, DMStyles::Border());

    SDL_Rect previous_clip{};
    SDL_GetRenderClipRect(renderer, &previous_clip);
    const bool was_clipping = SDL_RenderClipEnabled(renderer);

    SDL_SetRenderClipRect(renderer, &oval_list_clip_rect_);
    for (const auto& button : oval_buttons_) {
        if (button) {
            button->render(renderer);
        }
    }
    if (was_clipping) {
        SDL_SetRenderClipRect(renderer, &previous_clip);
    } else {
        SDL_SetRenderClipRect(renderer, nullptr);
    }

    if (add_oval_button_) add_oval_button_->render(renderer);
    if (has_selected_oval) {
        if (oval_name_textbox_) oval_name_textbox_->render(renderer);
        if (width_textbox_) width_textbox_->render(renderer);
        if (height_textbox_) height_textbox_->render(renderer);
        if (radius_offset_slider_) radius_offset_slider_->render(renderer);

        DMLabelStyle asset_status_style = label_style;
        std::string asset_status_text = "Attach Asset: <none>";
        switch (asset_binding_status_.kind) {
            case AssetBindingStatusKind::ExplicitCandidates:
                asset_status_style.color = SDL_Color{110, 210, 120, 240};
                asset_status_text = "Attach Asset: explicit candidates";
                if (!asset_binding_status_.detail.empty()) {
                    asset_status_text += " (" + asset_binding_status_.detail + ")";
                }
                break;
            case AssetBindingStatusKind::OvalFallback:
                asset_status_style.color = SDL_Color{110, 210, 120, 240};
                asset_status_text = "Attach Asset: oval fallback";
                if (!asset_binding_status_.detail.empty()) {
                    asset_status_text += " (" + asset_binding_status_.detail + ")";
                }
                break;
            case AssetBindingStatusKind::Missing:
                asset_status_style.color = SDL_Color{230, 140, 120, 240};
                asset_status_text = "Attach Asset: missing";
                if (!asset_binding_status_.detail.empty()) {
                    asset_status_text += " (" + asset_binding_status_.detail + ")";
                }
                break;
            case AssetBindingStatusKind::Invalid:
                asset_status_style.color = SDL_Color{230, 140, 120, 240};
                asset_status_text = "Attach Asset: invalid";
                if (!asset_binding_status_.detail.empty()) {
                    asset_status_text += " (" + asset_binding_status_.detail + ")";
                }
                break;
            case AssetBindingStatusKind::None:
            default:
                asset_status_style.color = SDL_Color{180, 190, 205, 220};
                if (!asset_binding_status_.detail.empty()) {
                    asset_status_text = "Attach Asset: " + asset_binding_status_.detail;
                }
                break;
        }
        DMFontCache::instance().draw_text(renderer,
                                          asset_status_style,
                                          asset_status_text,
                                          asset_status_rect_.x,
                                          asset_status_rect_.y);
        DMLabelStyle candidate_hint_style = label_style;
        candidate_hint_style.color = SDL_Color{150, 165, 190, 220};
        DMFontCache::instance().draw_text(renderer,
                                          candidate_hint_style,
                                          "Right-click an oval name to edit candidates.",
                                          candidate_hint_rect_.x,
                                          candidate_hint_rect_.y);
        if (apply_oval_properties_button_) apply_oval_properties_button_->render(renderer);

        const SDL_Color status_color = center_anchor_present_
            ? SDL_Color{110, 210, 120, 240}
            : SDL_Color{230, 140, 120, 240};
        const std::string center_text = center_anchor_name_.empty()
            ? std::string("Center Anchor: <none>")
            : std::string("Center Anchor: ") + center_anchor_name_ + (center_anchor_present_ ? " (Present)" : " (Missing)");
        DMLabelStyle status_style = label_style;
        status_style.color = status_color;
        DMFontCache::instance().draw_text(renderer, status_style, center_text, center_status_rect_.x, center_status_rect_.y);
        if (delete_oval_button_) delete_oval_button_->render(renderer);
    }

    if (point_count_stepper_) point_count_stepper_->render(renderer);

    if (has_selected_point) {
        if (advanced_card_rect_.w > 0 && advanced_card_rect_.h > 0) {
            const SDL_Color fill = dm_draw::LightenColor(DMStyles::PanelBG(), 0.04f);
            dm_draw::DrawBeveledRect(renderer,
                                     advanced_card_rect_,
                                     DMStyles::CornerRadius(),
                                     DMStyles::BevelDepth(),
                                     fill,
                                     DMStyles::HighlightColor(),
                                     DMStyles::ShadowColor(),
                                     false,
                                     DMStyles::HighlightIntensity(),
                                     DMStyles::ShadowIntensity());
            dm_draw::DrawRoundedOutline(renderer, advanced_card_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());
        }
    }

    if (has_selected_point) {
        DMFontCache::instance().draw_text(renderer, label_style, "Point Properties", point_detail_title_rect_.x, point_detail_title_rect_.y);
        if (point_rotation_slider_) point_rotation_slider_->render(renderer);
        if (point_hidden_checkbox_) point_hidden_checkbox_->render(renderer);
        if (advanced_options_button_) advanced_options_button_->render(renderer);
        if (advanced_options_expanded_) {
            if (point_resolve_x_checkbox_) point_resolve_x_checkbox_->render(renderer);
            if (point_scaling_method_dropdown_) point_scaling_method_dropdown_->render(renderer);
        }
    }

    if (has_selected_center && center_panel_rect_.w > 0 && center_panel_rect_.h > 0) {
        dm_draw::DrawBeveledRect(renderer,
                                 center_panel_rect_,
                                 DMStyles::CornerRadius(),
                                 DMStyles::BevelDepth(),
                                 bg,
                                 DMStyles::HighlightColor(),
                                 DMStyles::ShadowColor(),
                                 false,
                                 DMStyles::HighlightIntensity(),
                                 DMStyles::ShadowIntensity());
        dm_draw::DrawRoundedOutline(renderer, center_panel_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());
        DMFontCache::instance().draw_text(renderer, label_style, "Center Anchor Tools", center_header_rect_.x, center_header_rect_.y);

        if (advanced_card_rect_.w > 0 && advanced_card_rect_.h > 0) {
            const SDL_Color fill = dm_draw::LightenColor(DMStyles::PanelBG(), 0.04f);
            dm_draw::DrawBeveledRect(renderer,
                                     advanced_card_rect_,
                                     DMStyles::CornerRadius(),
                                     DMStyles::BevelDepth(),
                                     fill,
                                     DMStyles::HighlightColor(),
                                     DMStyles::ShadowColor(),
                                     false,
                                     DMStyles::HighlightIntensity(),
                                     DMStyles::ShadowIntensity());
            dm_draw::DrawRoundedOutline(renderer, advanced_card_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());
        }

        DMFontCache::instance().draw_text(renderer, label_style, "Center Anchor Properties", point_detail_title_rect_.x, point_detail_title_rect_.y);
        if (center_depth_textbox_) center_depth_textbox_->render(renderer);
        if (center_rotation_slider_) center_rotation_slider_->render(renderer);
        if (center_hidden_checkbox_) center_hidden_checkbox_->render(renderer);
        if (center_advanced_options_button_) center_advanced_options_button_->render(renderer);
        if (center_advanced_options_expanded_) {
            if (center_resolve_x_checkbox_) center_resolve_x_checkbox_->render(renderer);
            if (center_flip_vertical_checkbox_) center_flip_vertical_checkbox_->render(renderer);
            if (center_flip_horizontal_checkbox_) center_flip_horizontal_checkbox_->render(renderer);
            if (center_scaling_method_dropdown_) center_scaling_method_dropdown_->render(renderer);
        }
        if (center_apply_next_frame_button_) center_apply_next_frame_button_->render(renderer);
        if (center_apply_animation_button_) center_apply_animation_button_->render(renderer);
        if (center_apply_asset_button_) center_apply_asset_button_->render(renderer);
    }
}

bool RoomOvalToolsPanel::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    const_cast<RoomOvalToolsPanel*>(this)->update_layout();
    return point_in_rect(x, y, panel_rect_) || point_in_rect(x, y, center_panel_rect_);
}

void RoomOvalToolsPanel::update_layout() const {
    if (!layout_dirty_) {
        return;
    }

    const bool has_selected_oval = selected_oval_index_ >= 0 &&
                                   selected_oval_index_ < static_cast<int>(oval_names_.size());
    const bool has_selected_center = has_selected_oval && center_selected_ && center_anchor_present_;
    const bool has_selected_point = false;

    const int safe_w = std::max(screen_w_, 360);
    const int safe_h = std::max(screen_h_, 420);
    const int available_h = std::max(kPanelMinHeight, safe_h - kTopOffset - kPanelMargin);
    const int panel_h = std::clamp(available_h, kPanelMinHeight, kPanelMaxHeight);
    const int panel_gap = kPanelMargin;
    if (panel_bounds_override_active_) {
        panel_rect_ = panel_bounds_override_;
        if (panel_rect_.w <= 0) {
            panel_rect_.w = std::min(kPanelWidth, std::max(260, safe_w - kPanelMargin * 2));
        }
        if (panel_rect_.h <= 0) {
            panel_rect_.h = panel_h;
        }
        if (has_selected_center) {
            const int center_w = panel_rect_.w;
            int center_x = panel_rect_.x + panel_rect_.w + panel_gap;
            if (center_x + center_w > safe_w - kPanelMargin) {
                center_x = std::max(kPanelMargin, panel_rect_.x - panel_gap - center_w);
            }
            center_panel_rect_ = SDL_Rect{center_x, panel_rect_.y, center_w, panel_rect_.h};
        } else {
            center_panel_rect_ = SDL_Rect{0, 0, 0, 0};
        }
    } else {
        const int max_w = std::max(260, safe_w - kPanelMargin * 2);
        if (has_selected_center) {
            int panel_w = std::max(180, (max_w - panel_gap) / 2);
            if (panel_w * 2 + panel_gap > max_w) {
                panel_w = std::max(160, (max_w - panel_gap) / 2);
            }
            panel_rect_ = SDL_Rect{kPanelMargin, kTopOffset, panel_w, panel_h};
            center_panel_rect_ = SDL_Rect{panel_rect_.x + panel_rect_.w + panel_gap, kTopOffset, panel_w, panel_h};
        } else {
            const int panel_w = std::min(kPanelWidth, max_w);
            panel_rect_ = SDL_Rect{kPanelMargin, kTopOffset, panel_w, panel_h};
            center_panel_rect_ = SDL_Rect{0, 0, 0, 0};
        }
    }

    const int content_x = panel_rect_.x + kPanelPadding;
    const int content_w = std::max(0, panel_rect_.w - (kPanelPadding * 2));
    int y = panel_rect_.y + kPanelPadding;

    header_rect_ = SDL_Rect{content_x, y, content_w, kHeaderHeight};
    y += kHeaderHeight + kSectionGap;

    const int oval_rows = std::max(2, std::min(6, static_cast<int>(oval_names_.size()) + 1));
    const int oval_list_h = std::clamp(
        oval_rows * (DMButton::height() + DMSpacing::small_gap()) - DMSpacing::small_gap(),
        kListMinHeight,
        kListMaxHeight);
    oval_list_clip_rect_ = SDL_Rect{content_x, y, content_w, oval_list_h};
    y += oval_list_h + kSectionGap;

    if (add_oval_button_) {
        add_oval_button_->set_rect(SDL_Rect{content_x, y, content_w, DMButton::height()});
    }
    y += DMButton::height() + kSectionGap;

    if (has_selected_oval) {
        const int name_h = oval_name_textbox_ ? oval_name_textbox_->preferred_height(content_w) : DMTextBox::height();
        const int width_h = width_textbox_ ? width_textbox_->preferred_height(content_w) : DMTextBox::height();
        const int height_h = height_textbox_ ? height_textbox_->preferred_height(content_w) : DMTextBox::height();
        const int offset_h = radius_offset_slider_ ? radius_offset_slider_->preferred_height(content_w) : DMSlider::height();

        if (oval_name_textbox_) {
            oval_name_textbox_->set_rect(SDL_Rect{content_x, y, content_w, name_h});
        }
        y += name_h + kRowGap;

        const int split_gap = DMSpacing::small_gap();
        const int split_w = std::max(0, content_w - split_gap);
        const int left_w = split_w / 2;
        const int right_w = std::max(0, content_w - left_w - split_gap);
        const int row_h = std::max(width_h, height_h);
        if (width_textbox_) {
            width_textbox_->set_rect(SDL_Rect{content_x, y, left_w, row_h});
        }
        if (height_textbox_) {
            height_textbox_->set_rect(SDL_Rect{content_x + left_w + split_gap, y, right_w, row_h});
        }
        y += row_h + kRowGap;

        if (radius_offset_slider_) {
            radius_offset_slider_->set_rect(SDL_Rect{content_x, y, content_w, offset_h});
        }
        y += offset_h + kRowGap;

        asset_status_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};
        y += kLineHeight + kRowGap;

        candidate_hint_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};
        y += kLineHeight + kRowGap;

        if (apply_oval_properties_button_) {
            apply_oval_properties_button_->set_rect(SDL_Rect{content_x, y, content_w, DMButton::height()});
        }
        y += DMButton::height() + kRowGap;

        center_status_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};
        y += kLineHeight + kRowGap;

        if (delete_oval_button_) {
            delete_oval_button_->set_rect(SDL_Rect{content_x, y, content_w, DMButton::height()});
        }
        y += DMButton::height() + kSectionGap;
    } else {
        if (oval_name_textbox_) oval_name_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (width_textbox_) width_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (height_textbox_) height_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (radius_offset_slider_) radius_offset_slider_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (apply_oval_properties_button_) apply_oval_properties_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (delete_oval_button_) delete_oval_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        asset_status_rect_ = SDL_Rect{0, 0, 0, 0};
        candidate_hint_rect_ = SDL_Rect{0, 0, 0, 0};
        center_status_rect_ = SDL_Rect{0, 0, 0, 0};
    }

    const int stepper_h = point_count_stepper_ ? point_count_stepper_->preferred_height(content_w) : DMNumericStepper::height();
    if (point_count_stepper_) {
        point_count_stepper_->set_rect(SDL_Rect{content_x, y, content_w, stepper_h});
    }
    y += stepper_h + kSectionGap;

    point_detail_title_rect_ = SDL_Rect{0, 0, 0, 0};
    advanced_card_rect_ = SDL_Rect{0, 0, 0, 0};
    if (has_selected_point) {
        const int rotation_h = point_rotation_slider_ ? point_rotation_slider_->preferred_height(content_w) : DMSlider::height();
        const int hidden_h = point_hidden_checkbox_ ? DMCheckbox::height() : 0;
        const int advanced_h = advanced_options_button_ ? DMButton::height() : 0;
        const int resolve_h = point_resolve_x_checkbox_ ? DMCheckbox::height() : 0;
        const int scale_h = point_scaling_method_dropdown_
            ? point_scaling_method_dropdown_->preferred_height(content_w)
            : DMDropdown::height();

        point_detail_title_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};
        y += kLineHeight + kRowGap;
        if (point_rotation_slider_) {
            point_rotation_slider_->set_rect(SDL_Rect{content_x, y, content_w, rotation_h});
        }
        y += rotation_h + kRowGap;
        if (point_hidden_checkbox_) {
            point_hidden_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, hidden_h});
        }
        y += hidden_h + kRowGap;

        int advanced_top = y;
        if (advanced_options_button_) {
            advanced_options_button_->set_text(
                advanced_options_expanded_ ? "Advanced Options (Hide)" : "Advanced Options (Show)");
            advanced_options_button_->set_rect(SDL_Rect{content_x, y, content_w, advanced_h});
        }
        y += advanced_h;
        int advanced_bottom = y;
        if (advanced_options_expanded_) {
            y += kRowGap;
            if (point_resolve_x_checkbox_) {
                point_resolve_x_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, resolve_h});
                y += resolve_h + kRowGap;
            }
            if (point_scaling_method_dropdown_) {
                point_scaling_method_dropdown_->set_rect(SDL_Rect{content_x, y, content_w, scale_h});
                y += scale_h;
            }
            advanced_bottom = y;
        } else {
            if (point_resolve_x_checkbox_) point_resolve_x_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
            if (point_scaling_method_dropdown_) point_scaling_method_dropdown_->set_rect(SDL_Rect{0, 0, 0, 0});
        }

        const int card_top = std::max(panel_rect_.y + kPanelPadding, advanced_top - kAdvancedCardPadding);
        const int card_bottom = std::min(panel_rect_.y + panel_rect_.h - kPanelPadding, advanced_bottom + kAdvancedCardPadding);
        advanced_card_rect_ = SDL_Rect{content_x, card_top, content_w, std::max(0, card_bottom - card_top)};
    }

    if (!has_selected_point) {
        if (point_rotation_slider_) point_rotation_slider_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (point_hidden_checkbox_) point_hidden_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (advanced_options_button_) advanced_options_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (point_resolve_x_checkbox_) point_resolve_x_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (point_scaling_method_dropdown_) point_scaling_method_dropdown_->set_rect(SDL_Rect{0, 0, 0, 0});
    }

    if (has_selected_center && center_panel_rect_.w > 0 && center_panel_rect_.h > 0) {
        const int center_content_x = center_panel_rect_.x + kPanelPadding;
        const int center_content_w = std::max(0, center_panel_rect_.w - (kPanelPadding * 2));
        int center_y = center_panel_rect_.y + kPanelPadding;
        center_header_rect_ = SDL_Rect{center_content_x, center_y, center_content_w, kHeaderHeight};
        center_y += kHeaderHeight + kSectionGap;

        const int split_gap = DMSpacing::small_gap();
        const int split_w = std::max(0, center_content_w - split_gap);
        const int left_w = split_w / 2;
        const int right_w = std::max(0, center_content_w - left_w - split_gap);
        const int depth_h = center_depth_textbox_ ? center_depth_textbox_->preferred_height(center_content_w) : DMTextBox::height();
        const int rotation_h = center_rotation_slider_ ? center_rotation_slider_->preferred_height(center_content_w) : DMSlider::height();
        const int hidden_h = center_hidden_checkbox_ ? DMCheckbox::height() : 0;
        const int advanced_h = center_advanced_options_button_ ? DMButton::height() : 0;
        const int resolve_h = center_resolve_x_checkbox_ ? DMCheckbox::height() : 0;
        const int flip_h = std::max(center_flip_vertical_checkbox_ ? DMCheckbox::height() : 0,
                                    center_flip_horizontal_checkbox_ ? DMCheckbox::height() : 0);
        const int scale_h = center_scaling_method_dropdown_
            ? center_scaling_method_dropdown_->preferred_height(center_content_w)
            : DMDropdown::height();

        point_detail_title_rect_ = SDL_Rect{center_content_x, center_y, center_content_w, kLineHeight};
        center_y += kLineHeight + kRowGap;
        if (center_depth_textbox_) {
            center_depth_textbox_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, depth_h});
        }
        center_y += depth_h + kRowGap;
        if (center_rotation_slider_) {
            center_rotation_slider_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, rotation_h});
        }
        center_y += rotation_h + kRowGap;
        if (center_hidden_checkbox_) {
            center_hidden_checkbox_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, hidden_h});
        }
        center_y += hidden_h + kRowGap;

        int advanced_top = center_y;
        if (center_advanced_options_button_) {
            center_advanced_options_button_->set_text(
                center_advanced_options_expanded_ ? "Advanced Options (Hide)" : "Advanced Options (Show)");
            center_advanced_options_button_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, advanced_h});
        }
        center_y += advanced_h;
        int advanced_bottom = center_y;
        if (center_advanced_options_expanded_) {
            center_y += kRowGap;
            if (center_resolve_x_checkbox_) {
                center_resolve_x_checkbox_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, resolve_h});
                center_y += resolve_h + kRowGap;
            }
            if (center_flip_vertical_checkbox_ && center_flip_horizontal_checkbox_) {
                center_flip_vertical_checkbox_->set_rect(SDL_Rect{center_content_x, center_y, left_w, flip_h});
                center_flip_horizontal_checkbox_->set_rect(SDL_Rect{center_content_x + left_w + split_gap, center_y, right_w, flip_h});
            } else if (center_flip_vertical_checkbox_) {
                center_flip_vertical_checkbox_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, flip_h});
            } else if (center_flip_horizontal_checkbox_) {
                center_flip_horizontal_checkbox_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, flip_h});
            }
            center_y += flip_h + kRowGap;
            if (center_scaling_method_dropdown_) {
                center_scaling_method_dropdown_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, scale_h});
                center_y += scale_h;
            }
            advanced_bottom = center_y;
        } else {
            if (center_resolve_x_checkbox_) center_resolve_x_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
            if (center_flip_vertical_checkbox_) center_flip_vertical_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
            if (center_flip_horizontal_checkbox_) center_flip_horizontal_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
            if (center_scaling_method_dropdown_) center_scaling_method_dropdown_->set_rect(SDL_Rect{0, 0, 0, 0});
        }

        const int card_top = std::max(center_panel_rect_.y + kPanelPadding, advanced_top - kAdvancedCardPadding);
        const int card_bottom = std::min(center_panel_rect_.y + center_panel_rect_.h - kPanelPadding, advanced_bottom + kAdvancedCardPadding);
        advanced_card_rect_ = SDL_Rect{center_content_x, card_top, center_content_w, std::max(0, card_bottom - card_top)};

        center_y += kSectionGap;
        if (center_apply_next_frame_button_) {
            center_apply_next_frame_button_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, DMButton::height()});
        }
        center_y += DMButton::height() + kRowGap;
        if (center_apply_animation_button_) {
            center_apply_animation_button_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, DMButton::height()});
        }
        center_y += DMButton::height() + kRowGap;
        if (center_apply_asset_button_) {
            center_apply_asset_button_->set_rect(SDL_Rect{center_content_x, center_y, center_content_w, DMButton::height()});
        }
    } else {
        center_panel_rect_ = SDL_Rect{0, 0, 0, 0};
        center_header_rect_ = SDL_Rect{0, 0, 0, 0};
        if (center_depth_textbox_) center_depth_textbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_rotation_slider_) center_rotation_slider_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_hidden_checkbox_) center_hidden_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_advanced_options_button_) center_advanced_options_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_resolve_x_checkbox_) center_resolve_x_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_flip_vertical_checkbox_) center_flip_vertical_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_flip_horizontal_checkbox_) center_flip_horizontal_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_scaling_method_dropdown_) center_scaling_method_dropdown_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_apply_next_frame_button_) center_apply_next_frame_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_apply_animation_button_) center_apply_animation_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (center_apply_asset_button_) center_apply_asset_button_->set_rect(SDL_Rect{0, 0, 0, 0});
    }

    if (!has_selected_center && !has_selected_point) {
        point_detail_title_rect_ = SDL_Rect{0, 0, 0, 0};
        advanced_card_rect_ = SDL_Rect{0, 0, 0, 0};
    }

    const_cast<RoomOvalToolsPanel*>(this)->layout_oval_buttons();
    layout_dirty_ = false;
}

void RoomOvalToolsPanel::layout_oval_buttons() const {
    int y = oval_list_clip_rect_.y;
    const int w = std::max(0, oval_list_clip_rect_.w);
    for (std::size_t i = 0; i < oval_buttons_.size(); ++i) {
        DMButton* button = oval_buttons_[i].get();
        if (!button) {
            continue;
        }
        button->set_rect(SDL_Rect{oval_list_clip_rect_.x, y, w, DMButton::height()});
        const bool selected = static_cast<int>(i) == selected_oval_index_;
        button->set_style(selected ? &DMStyles::AccentButton() : &DMStyles::ListButton());
        y += DMButton::height() + DMSpacing::small_gap();
    }
}

bool RoomOvalToolsPanel::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point point{x, y};
    return SDL_PointInRect(&point, &rect);
}

float RoomOvalToolsPanel::parse_float_or(const std::string& text, float fallback) {
    try {
        const float value = std::stof(text);
        return std::isfinite(value) ? value : fallback;
    } catch (...) {
        return fallback;
    }
}

std::string RoomOvalToolsPanel::format_float(float value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    std::ostringstream oss;
    oss.setf(std::ios::fixed, std::ios::floatfield);
    oss << std::setprecision(3) << value;
    std::string formatted = oss.str();
    while (!formatted.empty() && formatted.back() == '0') {
        formatted.pop_back();
    }
    if (!formatted.empty() && formatted.back() == '.') {
        formatted.pop_back();
    }
    if (formatted.empty() || formatted == "-0") {
        return "0";
    }
    return formatted;
}

int RoomOvalToolsPanel::scaling_method_to_dropdown_index(AnchorScalingMethod method) {
    switch (method) {
        case AnchorScalingMethod::Parent:
            return 0;
        case AnchorScalingMethod::Real3DPoint:
            return 1;
        case AnchorScalingMethod::Relative2DAnchorPoint:
            return 2;
        case AnchorScalingMethod::Real3DFloorPoint:
            return 3;
    }
    return 0;
}

AnchorScalingMethod RoomOvalToolsPanel::dropdown_index_to_scaling_method(int index) {
    switch (index) {
        case 1:
            return AnchorScalingMethod::Real3DPoint;
        case 2:
            return AnchorScalingMethod::Relative2DAnchorPoint;
        case 3:
            return AnchorScalingMethod::Real3DFloorPoint;
        default:
            return AnchorScalingMethod::Parent;
    }
}

RoomOvalToolsPanel::PointDetailValues RoomOvalToolsPanel::collect_point_detail_values() const {
    PointDetailValues values{};
    values.rotation_degrees = point_rotation_slider_ ? static_cast<float>(point_rotation_slider_->value()) : 0.0f;
    values.hidden = point_hidden_checkbox_ ? point_hidden_checkbox_->value() : false;
    values.resolve_x = point_resolve_x_checkbox_ ? point_resolve_x_checkbox_->value() : true;
    values.scaling_method = dropdown_index_to_scaling_method(
        point_scaling_method_dropdown_ ? point_scaling_method_dropdown_->selected() : 0);
    if (!std::isfinite(values.rotation_degrees)) {
        values.rotation_degrees = 0.0f;
    }
    return values;
}

RoomOvalToolsPanel::CenterDetailValues RoomOvalToolsPanel::collect_center_detail_values() const {
    CenterDetailValues values{};
    values.depth_offset = parse_float_or(center_depth_textbox_ ? center_depth_textbox_->value() : std::string{}, 0.0f);
    values.rotation_degrees = center_rotation_slider_ ? static_cast<float>(center_rotation_slider_->value()) : 0.0f;
    values.hidden = center_hidden_checkbox_ ? center_hidden_checkbox_->value() : false;
    values.resolve_x = center_resolve_x_checkbox_ ? center_resolve_x_checkbox_->value() : true;
    values.flip_horizontal = center_flip_horizontal_checkbox_ ? center_flip_horizontal_checkbox_->value() : true;
    values.flip_vertical = center_flip_vertical_checkbox_ ? center_flip_vertical_checkbox_->value() : true;
    values.scaling_method = dropdown_index_to_scaling_method(
        center_scaling_method_dropdown_ ? center_scaling_method_dropdown_->selected() : 0);
    if (!std::isfinite(values.depth_offset)) {
        values.depth_offset = 0.0f;
    }
    if (!std::isfinite(values.rotation_degrees)) {
        values.rotation_degrees = 0.0f;
    }
    return values;
}

RoomOvalToolsPanel::OvalProperties RoomOvalToolsPanel::collect_oval_properties() const {
    OvalProperties values = oval_properties_;
    values.name = oval_name_textbox_ ? oval_name_textbox_->value() : values.name;
    values.width_radius_x = parse_float_or(width_textbox_ ? width_textbox_->value() : std::string{}, values.width_radius_x);
    values.height_radius_z = parse_float_or(height_textbox_ ? height_textbox_->value() : std::string{}, values.height_radius_z);
    values.radius_offset_degrees =
        radius_offset_slider_ ? static_cast<float>(radius_offset_slider_->value()) : values.radius_offset_degrees;
    if (!std::isfinite(values.width_radius_x)) {
        values.width_radius_x = 48.0f;
    }
    if (!std::isfinite(values.height_radius_z)) {
        values.height_radius_z = 24.0f;
    }
    if (!std::isfinite(values.radius_offset_degrees)) {
        values.radius_offset_degrees = 0.0f;
    }
    values.radius_offset_degrees = std::clamp(values.radius_offset_degrees, 0.0f, 360.0f);
    return values;
}
