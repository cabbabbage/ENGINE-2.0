#include "room_movement_tools_panel.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/widgets.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"

namespace {

constexpr int kPanelMargin = 12;
constexpr int kTopOffset = 56;
constexpr int kPanelWidth = 336;
constexpr int kPanelHeight = 420;
constexpr int kPanelPadding = 16;
constexpr int kHeaderHeight = 22;
constexpr int kHeaderToEnabledGap = 10;
constexpr int kSectionGap = 14;
constexpr int kFieldGap = 12;
constexpr int kButtonGap = 8;
constexpr int kHintHeight = 44;
constexpr int kHintLineGap = 20;
constexpr int kPathListGap = 6;

}  // namespace

RoomMovementToolsPanel::RoomMovementToolsPanel() {
    enabled_checkbox_ = std::make_unique<DMCheckbox>("Movement Enabled", false);
    smooth_checkbox_ = std::make_unique<DMCheckbox>("Smooth interpolation", false);
    curve_checkbox_ = std::make_unique<DMCheckbox>("Curve interpolation", false);
    dx_box_ = std::make_unique<DMTextBox>("DX", "0");
    dy_box_ = std::make_unique<DMTextBox>("DY", "0");
    dz_box_ = std::make_unique<DMTextBox>("DZ", "0");
    rot_box_ = std::make_unique<DMTextBox>("Rotation", "0");
    path_options_ = {"Path 1"};
    path_buttons_.push_back(std::make_unique<DMButton>("Path 1", &DMStyles::AccentButton(), 220, DMButton::height()));
    add_path_button_ = std::make_unique<DMButton>("+ New Path", &DMStyles::CreateButton(), 140, DMButton::height());
    delete_path_button_ = std::make_unique<DMButton>("Delete Path", &DMStyles::DeleteButton(), 140, DMButton::height());
}

RoomMovementToolsPanel::~RoomMovementToolsPanel() = default;

void RoomMovementToolsPanel::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    layout_dirty_ = true;
}

void RoomMovementToolsPanel::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void RoomMovementToolsPanel::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_bounds_override_ = bounds;
    panel_bounds_override_active_ = bounds.w > 0 && bounds.h > 0;
    layout_dirty_ = true;
}

void RoomMovementToolsPanel::clear_panel_bounds_override() {
    panel_bounds_override_active_ = false;
    panel_bounds_override_ = SDL_Rect{0, 0, 0, 0};
    layout_dirty_ = true;
}

void RoomMovementToolsPanel::set_smooth_enabled(bool enabled) {
    if (smooth_checkbox_) {
        smooth_checkbox_->set_value(enabled);
    }
    if (!enabled && curve_checkbox_) {
        curve_checkbox_->set_value(false);
    }
}

bool RoomMovementToolsPanel::smooth_enabled() const {
    return smooth_checkbox_ ? smooth_checkbox_->value() : false;
}

void RoomMovementToolsPanel::set_curve_enabled(bool enabled) {
    if (!enabled) {
        if (curve_checkbox_) {
            curve_checkbox_->set_value(false);
        }
        return;
    }
    if (smooth_checkbox_) {
        smooth_checkbox_->set_value(true);
    }
    if (curve_checkbox_) {
        curve_checkbox_->set_value(true);
    }
}

bool RoomMovementToolsPanel::curve_enabled() const {
    return curve_checkbox_ ? curve_checkbox_->value() : false;
}

void RoomMovementToolsPanel::set_system_enabled(bool enabled) {
    if (enabled_checkbox_) {
        enabled_checkbox_->set_value(enabled);
    }
}

bool RoomMovementToolsPanel::system_enabled() const {
    return enabled_checkbox_ ? enabled_checkbox_->value() : false;
}

void RoomMovementToolsPanel::set_numeric_values(const NumericValues& values) {
    if (dx_box_ && !dx_box_->is_editing()) {
        dx_box_->set_value(std::to_string(static_cast<int>(std::lround(values.dx))));
    }
    if (dy_box_ && !dy_box_->is_editing()) {
        dy_box_->set_value(std::to_string(static_cast<int>(std::lround(values.dy))));
    }
    if (dz_box_ && !dz_box_->is_editing()) {
        dz_box_->set_value(std::to_string(static_cast<int>(std::lround(values.dz))));
    }
    if (rot_box_ && !rot_box_->is_editing()) {
        rot_box_->set_value(std::to_string(values.rotation_degrees));
    }
}

RoomMovementToolsPanel::NumericValues RoomMovementToolsPanel::numeric_values() const {
    auto parse_or = [](const DMTextBox* box, float fallback) {
        if (!box) {
            return fallback;
        }
        try {
            return std::stof(box->value());
        } catch (...) {
            return fallback;
        }
    };

    NumericValues values{};
    values.dx = parse_or(dx_box_.get(), 0.0f);
    values.dy = parse_or(dy_box_.get(), 0.0f);
    values.dz = parse_or(dz_box_.get(), 0.0f);
    values.rotation_degrees = parse_or(rot_box_.get(), 0.0f);
    if (!std::isfinite(values.rotation_degrees)) {
        values.rotation_degrees = 0.0f;
    }
    return values;
}

bool RoomMovementToolsPanel::any_numeric_editing() const {
    return (dx_box_ && dx_box_->is_editing()) ||
           (dy_box_ && dy_box_->is_editing()) ||
           (dz_box_ && dz_box_->is_editing()) ||
           (rot_box_ && rot_box_->is_editing());
}

void RoomMovementToolsPanel::set_on_system_enabled_toggle(SystemEnabledToggleCallback callback) {
    on_system_enabled_toggle_ = std::move(callback);
}
void RoomMovementToolsPanel::set_path_options(const std::vector<std::string>& options, int selected_index) {
    path_options_ = options.empty() ? std::vector<std::string>{"Path 1"} : options;
    selected_path_index_ = std::clamp(selected_index, 0, static_cast<int>(path_options_.size()) - 1);
    path_buttons_.clear();
    path_buttons_.reserve(path_options_.size());
    for (std::size_t i = 0; i < path_options_.size(); ++i) {
        const bool selected = static_cast<int>(i) == selected_path_index_;
        path_buttons_.push_back(std::make_unique<DMButton>(path_options_[i],
                                                           selected ? &DMStyles::AccentButton() : &DMStyles::ListButton(),
                                                           220,
                                                           DMButton::height()));
    }
    layout_dirty_ = true;
}
void RoomMovementToolsPanel::set_on_path_selection_changed(PathSelectionChangedCallback callback) { on_path_selection_changed_ = std::move(callback); }
void RoomMovementToolsPanel::set_on_add_path(PathActionCallback callback) { on_add_path_ = std::move(callback); }
void RoomMovementToolsPanel::set_on_delete_path(PathActionCallback callback) { on_delete_path_ = std::move(callback); }

bool RoomMovementToolsPanel::handle_event(const SDL_Event& event) {
    if (!visible_) {
        return false;
    }

    update_layout();

    bool handled = false;
    if (enabled_checkbox_) {
        const bool before = enabled_checkbox_->value();
        if (enabled_checkbox_->handle_event(event)) {
            handled = true;
            const bool after = enabled_checkbox_->value();
            if (before != after && on_system_enabled_toggle_) {
                on_system_enabled_toggle_(after);
            }
        }
    }

    if (!system_enabled()) {
        if (handled) {
            return true;
        }
    } else {
        if (smooth_checkbox_ && smooth_checkbox_->handle_event(event)) {
            handled = true;
        }
        if (curve_checkbox_ && curve_checkbox_->handle_event(event)) {
            handled = true;
        }
        if (dx_box_ && dx_box_->handle_event(event)) {
            handled = true;
        }
        if (dy_box_ && dy_box_->handle_event(event)) {
            handled = true;
        }
        if (dz_box_ && dz_box_->handle_event(event)) {
            handled = true;
        }
        if (rot_box_ && rot_box_->handle_event(event)) {
            handled = true;
        }
        for (std::size_t i = 0; i < path_buttons_.size(); ++i) {
            auto& button = path_buttons_[i];
            if (!button) continue;
            if (button->handle_event(event)) {
                handled = true;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
                    selected_path_index_ = static_cast<int>(i);
                    for (std::size_t j = 0; j < path_buttons_.size(); ++j) {
                        if (!path_buttons_[j]) continue;
                        path_buttons_[j]->set_style(static_cast<int>(j) == selected_path_index_
                                                        ? &DMStyles::AccentButton()
                                                        : &DMStyles::ListButton());
                    }
                    if (on_path_selection_changed_) on_path_selection_changed_(selected_path_index_);
                }
            }
        }
        if (add_path_button_ && add_path_button_->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && on_add_path_) on_add_path_();
        }
        if (delete_path_button_ && delete_path_button_->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && on_delete_path_) on_delete_path_();
        }

        if (!smooth_enabled()) {
            set_curve_enabled(false);
        }

        if (handled) {
            return true;
        }
    }

    SDL_Point pointer{0, 0};
    const bool pointer_event =
        event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    const bool wheel_event = event.type == SDL_EVENT_MOUSE_WHEEL;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        pointer = sdl_mouse_util::MotionPoint(event.motion);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        pointer = sdl_mouse_util::ButtonPoint(event.button);
    } else if (wheel_event) {
        sdl_mouse_util::GetMouseState(&pointer.x, &pointer.y);
    }

    if ((pointer_event || wheel_event) && point_in_rect(pointer.x, pointer.y, panel_rect_)) {
        return true;
    }
    return false;
}

void RoomMovementToolsPanel::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) {
        return;
    }

    update_layout();

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect(renderer,
                             panel_rect_,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             true,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, panel_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());

    const DMLabelStyle& label_style = DMStyles::Label();
    DMFontCache::instance().draw_text(renderer, label_style, "Movement Editor", header_rect_.x, header_rect_.y);
    if (enabled_checkbox_) {
        enabled_checkbox_->render(renderer);
    }

    if (!system_enabled()) {
        return;
    }

    DMFontCache::instance().draw_text(renderer, label_style, "Drag selected point on ground", hint_rect_.x, hint_rect_.y);
    DMFontCache::instance().draw_text(renderer, label_style, "Mouse wheel adjusts height", hint_rect_.x, hint_rect_.y + kHintLineGap);

    if (smooth_checkbox_) {
        smooth_checkbox_->render(renderer);
    }
    if (curve_checkbox_) {
        curve_checkbox_->render(renderer);
    }
    if (dx_box_) {
        dx_box_->render(renderer);
    }
    if (dy_box_) {
        dy_box_->render(renderer);
    }
    if (dz_box_) {
        dz_box_->render(renderer);
    }
    if (rot_box_) {
        rot_box_->render(renderer);
    }
    for (const auto& button : path_buttons_) {
        if (button) button->render(renderer);
    }
    if (add_path_button_) add_path_button_->render(renderer);
    if (delete_path_button_) delete_path_button_->render(renderer);
}

bool RoomMovementToolsPanel::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    update_layout();
    return point_in_rect(x, y, panel_rect_);
}

void RoomMovementToolsPanel::update_layout() const {
    if (!layout_dirty_) {
        return;
    }

    if (panel_bounds_override_active_) {
        panel_rect_ = panel_bounds_override_;
    } else {
        panel_rect_ = SDL_Rect{kPanelMargin, kTopOffset, kPanelWidth, kPanelHeight};
    }

    panel_rect_.w = std::max(panel_rect_.w, 0);
    panel_rect_.h = std::max(panel_rect_.h, 0);

    const int content_x = panel_rect_.x + kPanelPadding;
    const int content_w = std::max(0, panel_rect_.w - kPanelPadding * 2);
    int cursor_y = panel_rect_.y + kPanelPadding;

    header_rect_ = SDL_Rect{content_x, cursor_y, content_w, kHeaderHeight};
    cursor_y += header_rect_.h + kHeaderToEnabledGap;

    enabled_rect_ = SDL_Rect{content_x, cursor_y, content_w, DMCheckbox::height()};
    cursor_y += enabled_rect_.h + kSectionGap;

    hint_rect_ = SDL_Rect{content_x, cursor_y, content_w, kHintHeight};
    cursor_y += hint_rect_.h + kSectionGap;

    smooth_rect_ = SDL_Rect{content_x, cursor_y, content_w, DMCheckbox::height()};
    cursor_y += smooth_rect_.h + kFieldGap;

    curve_rect_ = SDL_Rect{content_x, cursor_y, content_w, DMCheckbox::height()};
    cursor_y += curve_rect_.h + kSectionGap;

    const auto textbox_height_for = [content_w](const std::unique_ptr<DMTextBox>& box) {
        if (!box) {
            return DMTextBox::height();
        }
        return std::max(DMTextBox::height(), box->height_for_width(content_w));
    };

    dx_rect_ = SDL_Rect{content_x, cursor_y, content_w, textbox_height_for(dx_box_)};
    cursor_y += dx_rect_.h + kFieldGap;

    dy_rect_ = SDL_Rect{content_x, cursor_y, content_w, textbox_height_for(dy_box_)};
    cursor_y += dy_rect_.h + kFieldGap;

    dz_rect_ = SDL_Rect{content_x, cursor_y, content_w, textbox_height_for(dz_box_)};
    cursor_y += dz_rect_.h + kFieldGap;

    rot_rect_ = SDL_Rect{content_x, cursor_y, content_w, textbox_height_for(rot_box_)};
    cursor_y += rot_rect_.h + kSectionGap;
    path_select_rect_ = SDL_Rect{content_x, cursor_y, content_w, DMButton::height()};
    const int path_list_h = static_cast<int>(path_buttons_.size()) * (DMButton::height() + kPathListGap) - (path_buttons_.empty() ? 0 : kPathListGap);
    path_list_rect_ = SDL_Rect{content_x, cursor_y, content_w, std::max(path_select_rect_.h, path_list_h)};
    cursor_y += path_list_rect_.h + kFieldGap;
    const int button_w = (content_w - kButtonGap) / 2;
    path_add_rect_ = SDL_Rect{content_x, cursor_y, std::max(0, button_w), DMButton::height()};
    path_delete_rect_ = SDL_Rect{content_x + std::max(0, button_w) + kButtonGap, cursor_y, std::max(0, button_w), DMButton::height()};
    cursor_y += std::max(path_add_rect_.h, path_delete_rect_.h) + kPanelPadding;

    if (!panel_bounds_override_active_) {
        panel_rect_.h = std::max(panel_rect_.h, cursor_y - panel_rect_.y);
    }

    if (enabled_checkbox_) {
        enabled_checkbox_->set_rect(enabled_rect_);
    }
    if (smooth_checkbox_) {
        smooth_checkbox_->set_rect(smooth_rect_);
    }
    if (curve_checkbox_) {
        curve_checkbox_->set_rect(curve_rect_);
    }
    if (dx_box_) {
        dx_box_->set_rect(dx_rect_);
    }
    if (dy_box_) {
        dy_box_->set_rect(dy_rect_);
    }
    if (dz_box_) {
        dz_box_->set_rect(dz_rect_);
    }
    if (rot_box_) {
        rot_box_->set_rect(rot_rect_);
    }
    int path_button_y = path_list_rect_.y;
    for (std::size_t i = 0; i < path_buttons_.size(); ++i) {
        auto& button = path_buttons_[i];
        if (!button) continue;
        button->set_rect(SDL_Rect{path_list_rect_.x, path_button_y, path_list_rect_.w, DMButton::height()});
        button->set_style(static_cast<int>(i) == selected_path_index_ ? &DMStyles::AccentButton() : &DMStyles::ListButton());
        path_button_y += DMButton::height() + kPathListGap;
    }
    if (add_path_button_) add_path_button_->set_rect(path_add_rect_);
    if (delete_path_button_) delete_path_button_->set_rect(path_delete_rect_);

    layout_dirty_ = false;
}

bool RoomMovementToolsPanel::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point point{x, y};
    return SDL_PointInRect(&point, &rect);
}
