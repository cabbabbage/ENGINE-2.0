#include "room_oval_point_child_editorpanel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>
#include <utility>

#include "dm_styles.hpp"
#include "draw_utils.hpp"
#include "font_cache.hpp"
#include "utils/string_utils.hpp"
#include "widgets.hpp"

namespace {

constexpr int kPanelMargin = 12;
constexpr int kTopOffset = 56;
constexpr int kPanelWidth = 332;
constexpr int kPanelMinHeight = 380;
constexpr int kPanelMaxHeight = 900;
constexpr int kPanelPadding = 10;
constexpr int kSectionGap = 8;
constexpr int kRowGap = 4;
constexpr int kHeaderHeight = 24;
constexpr int kLineHeight = 18;
constexpr int kChipWidth = 130;

const std::vector<std::string>& scaling_method_options() {
    static const std::vector<std::string> options{
        "Parent",
        "Real 3D Point",
        "Relative 2D Anchor Point",
        "Real 3D Floor Point",
    };
    return options;
}

std::vector<std::string> sorted_set_values(const std::set<std::string>& values) {
    return std::vector<std::string>(values.begin(), values.end());
}

}  // namespace

RoomOvalPointChildEditorPanel::RoomOvalPointChildEditorPanel() {
    point_rotation_slider_ = std::make_unique<DMSlider>("Rotation Degrees", -360, 360, 0);
    point_hidden_checkbox_ = std::make_unique<DMCheckbox>("Hidden", false);
    advanced_options_button_ = std::make_unique<DMButton>("Advanced Options (Show)", &DMStyles::ListButton(), 180, DMButton::height());
    point_resolve_x_checkbox_ = std::make_unique<DMCheckbox>("Resolve X", true);
    point_scaling_method_dropdown_ = std::make_unique<DMDropdown>("Scaling Method", scaling_method_options(), 0);
    search_box_ = std::make_unique<DMTextBox>("Find Tag", "");
    refresh_recommendations();
    rebuild_tag_buttons();
}

RoomOvalPointChildEditorPanel::~RoomOvalPointChildEditorPanel() = default;

void RoomOvalPointChildEditorPanel::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanel::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanel::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_bounds_override_ = bounds;
    panel_bounds_override_active_ = bounds.w > 0 && bounds.h > 0;
    layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanel::clear_panel_bounds_override() {
    panel_bounds_override_active_ = false;
    panel_bounds_override_ = SDL_Rect{0, 0, 0, 0};
    layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanel::set_point_selected(bool selected) {
    if (point_selected_ == selected) {
        return;
    }
    point_selected_ = selected;
    layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanel::set_resolved_asset_name(const std::string& asset_name) {
    if (resolved_asset_name_ == asset_name) {
        return;
    }
    resolved_asset_name_ = asset_name;
    layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanel::set_point_detail_values(const PointDetailValues& values) {
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
    set_tag_values(values.tags, values.anti_tags);
}

void RoomOvalPointChildEditorPanel::set_recommendation_pool(const std::vector<std::string>& tags) {
    std::set<std::string> deduped;
    for (const auto& raw : tags) {
        const std::string normalized = normalize_tag(raw);
        if (!normalized.empty()) {
            deduped.insert(normalized);
        }
    }
    const std::vector<std::string> normalized_pool(deduped.begin(), deduped.end());
    if (recommendation_pool_ == normalized_pool) {
        return;
    }
    recommendation_pool_ = normalized_pool;
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanel::set_on_apply_point_details(ApplyPointDetailsCallback callback) {
    on_apply_point_details_ = std::move(callback);
}

bool RoomOvalPointChildEditorPanel::handle_event(const SDL_Event& event) {
    if (!visible_) {
        return false;
    }
    update_layout();

    bool handled = false;
    int pointer_x = 0;
    int pointer_y = 0;
    const bool pointer_event = event.type == SDL_EVENT_MOUSE_MOTION ||
                               event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                               event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        pointer_x = event.motion.x;
        pointer_y = event.motion.y;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        pointer_x = event.button.x;
        pointer_y = event.button.y;
    }

    if (search_box_ && search_box_->handle_event(event)) {
        handled = true;
        const std::string value = search_box_->value();
        if (value != search_input_) {
            search_input_ = value;
            search_query_ = to_lower_copy(search_input_);
            refresh_recommendations();
            rebuild_tag_buttons();
            layout_dirty_ = true;
        }
    }

    if (!point_selected_) {
        return handled || (pointer_event && is_point_inside(pointer_x, pointer_y));
    }

    bool details_changed = false;
    if (point_rotation_slider_ && point_rotation_slider_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (point_hidden_checkbox_ && point_hidden_checkbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (advanced_options_button_ && advanced_options_button_->handle_event(event)) {
        handled = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            advanced_options_expanded_ = !advanced_options_expanded_;
            layout_dirty_ = true;
        }
    }
    if (advanced_options_expanded_ && point_resolve_x_checkbox_ && point_resolve_x_checkbox_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }
    if (advanced_options_expanded_ && point_scaling_method_dropdown_ && point_scaling_method_dropdown_->handle_event(event)) {
        handled = true;
        details_changed = true;
    }

    std::optional<std::string> add_positive;
    std::optional<std::string> add_negative;
    std::optional<std::string> remove_positive;
    std::optional<std::string> remove_negative;

    for (auto& chip : recommended_tag_chips_) {
        if (!chip.button) {
            continue;
        }
        if (chip.button->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
                add_positive = chip.value;
            }
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_RIGHT &&
            point_in_rect(pointer_x, pointer_y, chip.button->rect())) {
            handled = true;
            add_negative = chip.value;
        }
    }
    for (auto& chip : positive_tag_chips_) {
        if (!chip.button) {
            continue;
        }
        if (chip.button->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
                remove_positive = chip.value;
            }
        }
    }
    for (auto& chip : negative_tag_chips_) {
        if (!chip.button) {
            continue;
        }
        if (chip.button->handle_event(event)) {
            handled = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
                remove_negative = chip.value;
            }
        }
    }

    if (add_positive.has_value()) {
        add_positive_tag(*add_positive);
    }
    if (add_negative.has_value()) {
        add_negative_tag(*add_negative);
    }
    if (remove_positive.has_value()) {
        remove_positive_tag(*remove_positive);
    }
    if (remove_negative.has_value()) {
        remove_negative_tag(*remove_negative);
    }

    if (details_changed) {
        emit_point_detail_change();
    }

    return handled || (pointer_event && is_point_inside(pointer_x, pointer_y));
}

void RoomOvalPointChildEditorPanel::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) {
        return;
    }
    update_layout();
    if (panel_rect_.w <= 0 || panel_rect_.h <= 0) {
        return;
    }

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
                                      DMStyles::Label(),
                                      "Oval Point Child",
                                      header_rect_.x,
                                      header_rect_.y);
    if (!resolved_asset_name_.empty()) {
        DMFontCache::instance().draw_text(renderer,
                                          label_style,
                                          "Child: " + resolved_asset_name_,
                                          source_rect_.x,
                                          source_rect_.y);
    }

    if (!point_selected_) {
        DMFontCache::instance().draw_text(renderer,
                                          label_style,
                                          "Select an oval point to edit child behavior.",
                                          title_rect_.x,
                                          title_rect_.y);
        return;
    }

    if (point_rotation_slider_) point_rotation_slider_->render(renderer);
    if (point_hidden_checkbox_) point_hidden_checkbox_->render(renderer);
    if (advanced_options_button_) advanced_options_button_->render(renderer);
    if (advanced_options_expanded_) {
        if (point_resolve_x_checkbox_) point_resolve_x_checkbox_->render(renderer);
        if (point_scaling_method_dropdown_) point_scaling_method_dropdown_->render(renderer);
    }
    if (search_box_) search_box_->render(renderer);

    DMFontCache::instance().draw_text(renderer, label_style, "Positive Tags", positive_label_rect_.x, positive_label_rect_.y);
    DMFontCache::instance().draw_text(renderer, label_style, "Negative Tags", negative_label_rect_.x, negative_label_rect_.y);
    DMFontCache::instance().draw_text(renderer, label_style, "Recommended Tags", recommended_label_rect_.x, recommended_label_rect_.y);

    for (const auto& chip : positive_tag_chips_) {
        if (chip.button) {
            chip.button->render(renderer);
        }
    }
    for (const auto& chip : negative_tag_chips_) {
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

bool RoomOvalPointChildEditorPanel::is_point_inside(int x, int y) const {
    if (!visible_) {
        return false;
    }
    update_layout();
    return point_in_rect(x, y, panel_rect_);
}

bool RoomOvalPointChildEditorPanel::point_in_rect(int x, int y, const SDL_Rect& rect) {
    SDL_Point point{x, y};
    return SDL_PointInRect(&point, &rect);
}

int RoomOvalPointChildEditorPanel::scaling_method_to_dropdown_index(AnchorScalingMethod method) {
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

AnchorScalingMethod RoomOvalPointChildEditorPanel::dropdown_index_to_scaling_method(int index) {
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

std::string RoomOvalPointChildEditorPanel::normalize_tag(std::string_view raw) {
    const std::string lowered = to_lower_copy(vibble::strings::trim_copy(std::string(raw)));
    if (lowered.empty()) {
        return {};
    }
    return lowered;
}

std::string RoomOvalPointChildEditorPanel::to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool RoomOvalPointChildEditorPanel::starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

void RoomOvalPointChildEditorPanel::update_layout() const {
    if (!layout_dirty_) {
        return;
    }

    if (panel_bounds_override_active_) {
        panel_rect_ = panel_bounds_override_;
    } else {
        const int height_available = std::max(0, screen_h_ - (kTopOffset + kPanelMargin));
        const int panel_height = std::clamp(height_available, kPanelMinHeight, kPanelMaxHeight);
        panel_rect_ = SDL_Rect{kPanelMargin + 340 + kPanelMargin, kTopOffset, kPanelWidth, panel_height};
    }

    const int content_x = panel_rect_.x + kPanelPadding;
    const int content_w = std::max(0, panel_rect_.w - (kPanelPadding * 2));
    int y = panel_rect_.y + kPanelPadding;

    header_rect_ = SDL_Rect{content_x, y, content_w, kHeaderHeight};
    y += kHeaderHeight + kRowGap;

    source_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};
    y += kLineHeight + kSectionGap;

    title_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};

    if (!point_selected_) {
        if (point_rotation_slider_) point_rotation_slider_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (point_hidden_checkbox_) point_hidden_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (advanced_options_button_) advanced_options_button_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (point_resolve_x_checkbox_) point_resolve_x_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (point_scaling_method_dropdown_) point_scaling_method_dropdown_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (search_box_) search_box_->set_rect(SDL_Rect{0, 0, 0, 0});
        layout_dirty_ = false;
        return;
    }

    y += kLineHeight + kRowGap;

    const int rotation_h = point_rotation_slider_ ? point_rotation_slider_->preferred_height(content_w) : DMSlider::height();
    const int hidden_h = point_hidden_checkbox_ ? DMCheckbox::height() : 0;
    const int advanced_h = advanced_options_button_ ? DMButton::height() : 0;
    const int resolve_h = point_resolve_x_checkbox_ ? DMCheckbox::height() : 0;
    const int scale_h = point_scaling_method_dropdown_
        ? point_scaling_method_dropdown_->preferred_height(content_w)
        : DMDropdown::height();

    if (point_rotation_slider_) point_rotation_slider_->set_rect(SDL_Rect{content_x, y, content_w, rotation_h});
    y += rotation_h + kRowGap;
    if (point_hidden_checkbox_) point_hidden_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, hidden_h});
    y += hidden_h + kRowGap;
    if (advanced_options_button_) {
        advanced_options_button_->set_text(advanced_options_expanded_ ? "Advanced Options (Hide)" : "Advanced Options (Show)");
        advanced_options_button_->set_rect(SDL_Rect{content_x, y, content_w, advanced_h});
    }
    y += advanced_h;
    if (advanced_options_expanded_) {
        y += kRowGap;
        if (point_resolve_x_checkbox_) {
            point_resolve_x_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, resolve_h});
            y += resolve_h + kRowGap;
        }
        if (point_scaling_method_dropdown_) {
            point_scaling_method_dropdown_->set_rect(SDL_Rect{content_x, y, content_w, scale_h});
            y += scale_h + kSectionGap;
        }
    } else {
        if (point_resolve_x_checkbox_) point_resolve_x_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        if (point_scaling_method_dropdown_) point_scaling_method_dropdown_->set_rect(SDL_Rect{0, 0, 0, 0});
        y += kSectionGap;
    }

    if (search_box_) {
        search_box_->set_rect(SDL_Rect{content_x, y, content_w, DMTextBox::height()});
    }
    y += DMTextBox::height() + kSectionGap;

    positive_label_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};
    y += kLineHeight + kRowGap;
    y = layout_tag_chips(const_cast<std::vector<TagChip>&>(positive_tag_chips_), content_x, content_w, y, true);
    y += kSectionGap;

    negative_label_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};
    y += kLineHeight + kRowGap;
    y = layout_tag_chips(const_cast<std::vector<TagChip>&>(negative_tag_chips_), content_x, content_w, y, true);
    y += kSectionGap;

    recommended_label_rect_ = SDL_Rect{content_x, y, content_w, kLineHeight};
    y += kLineHeight + kRowGap;
    layout_tag_chips(const_cast<std::vector<TagChip>&>(recommended_tag_chips_), content_x, content_w, y, true);

    layout_dirty_ = false;
}

int RoomOvalPointChildEditorPanel::layout_tag_chips(std::vector<TagChip>& chips,
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

void RoomOvalPointChildEditorPanel::set_tag_values(const std::vector<std::string>& positive,
                                                    const std::vector<std::string>& negative) {
    std::set<std::string> normalized_positive;
    std::set<std::string> normalized_negative;

    for (const auto& raw : positive) {
        const std::string normalized = normalize_tag(raw);
        if (!normalized.empty()) {
            normalized_positive.insert(normalized);
        }
    }
    for (const auto& raw : negative) {
        const std::string normalized = normalize_tag(raw);
        if (!normalized.empty() && normalized_positive.count(normalized) == 0) {
            normalized_negative.insert(normalized);
        }
    }

    if (normalized_positive == positive_tags_ && normalized_negative == negative_tags_) {
        return;
    }

    positive_tags_ = std::move(normalized_positive);
    negative_tags_ = std::move(normalized_negative);
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanel::refresh_recommendations() {
    std::vector<std::string> exact;
    std::vector<std::string> prefix;
    std::vector<std::string> contains;
    std::set<std::string> seen;

    const std::string normalized_query = normalize_tag(search_query_);
    for (const auto& tag : recommendation_pool_) {
        if (positive_tags_.count(tag) != 0 || negative_tags_.count(tag) != 0 || seen.count(tag) != 0) {
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
        positive_tags_.count(normalized_query) == 0 &&
        negative_tags_.count(normalized_query) == 0 &&
        std::find(recommended_tags_.begin(), recommended_tags_.end(), normalized_query) == recommended_tags_.end()) {
        recommended_tags_.insert(recommended_tags_.begin(), normalized_query);
    }
}

void RoomOvalPointChildEditorPanel::rebuild_tag_buttons() {
    positive_tag_chips_.clear();
    negative_tag_chips_.clear();
    recommended_tag_chips_.clear();

    for (const auto& value : positive_tags_) {
        TagChip chip;
        chip.value = value;
        chip.kind = TagKind::Positive;
        chip.button = std::make_unique<DMButton>(value, &DMStyles::HeaderButton(), kChipWidth, DMButton::height());
        positive_tag_chips_.push_back(std::move(chip));
    }
    for (const auto& value : negative_tags_) {
        TagChip chip;
        chip.value = value;
        chip.kind = TagKind::Negative;
        chip.button = std::make_unique<DMButton>(value, &DMStyles::DeleteButton(), kChipWidth, DMButton::height());
        negative_tag_chips_.push_back(std::move(chip));
    }
    for (const auto& value : recommended_tags_) {
        TagChip chip;
        chip.value = value;
        chip.kind = TagKind::Recommended;
        chip.button = std::make_unique<DMButton>(value, &DMStyles::AccentButton(), kChipWidth, DMButton::height());
        recommended_tag_chips_.push_back(std::move(chip));
    }
}

void RoomOvalPointChildEditorPanel::emit_point_detail_change() {
    if (!on_apply_point_details_) {
        return;
    }
    on_apply_point_details_(collect_point_detail_values());
}

RoomOvalPointChildEditorPanel::PointDetailValues RoomOvalPointChildEditorPanel::collect_point_detail_values() const {
    PointDetailValues values{};
    values.rotation_degrees = point_rotation_slider_ ? static_cast<float>(point_rotation_slider_->value()) : 0.0f;
    values.hidden = point_hidden_checkbox_ ? point_hidden_checkbox_->value() : false;
    values.resolve_x = point_resolve_x_checkbox_ ? point_resolve_x_checkbox_->value() : true;
    values.scaling_method = dropdown_index_to_scaling_method(
        point_scaling_method_dropdown_ ? point_scaling_method_dropdown_->selected() : 0);
    values.tags = sorted_set_values(positive_tags_);
    values.anti_tags = sorted_set_values(negative_tags_);
    return values;
}

void RoomOvalPointChildEditorPanel::add_positive_tag(const std::string& tag) {
    const std::string normalized = normalize_tag(tag);
    if (normalized.empty()) {
        return;
    }
    const std::size_t removed = negative_tags_.erase(normalized);
    const bool inserted = positive_tags_.insert(normalized).second;
    if (!inserted && removed == 0) {
        return;
    }
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
    emit_point_detail_change();
}

void RoomOvalPointChildEditorPanel::add_negative_tag(const std::string& tag) {
    const std::string normalized = normalize_tag(tag);
    if (normalized.empty()) {
        return;
    }
    const std::size_t removed = positive_tags_.erase(normalized);
    const bool inserted = negative_tags_.insert(normalized).second;
    if (!inserted && removed == 0) {
        return;
    }
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
    emit_point_detail_change();
}

void RoomOvalPointChildEditorPanel::remove_positive_tag(const std::string& tag) {
    const std::string normalized = normalize_tag(tag);
    if (normalized.empty() || positive_tags_.erase(normalized) == 0) {
        return;
    }
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
    emit_point_detail_change();
}

void RoomOvalPointChildEditorPanel::remove_negative_tag(const std::string& tag) {
    const std::string normalized = normalize_tag(tag);
    if (normalized.empty() || negative_tags_.erase(normalized) == 0) {
        return;
    }
    refresh_recommendations();
    rebuild_tag_buttons();
    layout_dirty_ = true;
    emit_point_detail_change();
}

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
void RoomOvalPointChildEditorPanelTestAccess::set_query(RoomOvalPointChildEditorPanel& panel, const std::string& query) {
    panel.search_input_ = query;
    panel.search_query_ = RoomOvalPointChildEditorPanel::to_lower_copy(query);
    if (panel.search_box_) {
        panel.search_box_->set_value(query);
    }
    panel.refresh_recommendations();
    panel.rebuild_tag_buttons();
    panel.layout_dirty_ = true;
}

void RoomOvalPointChildEditorPanelTestAccess::set_recommendation_pool(RoomOvalPointChildEditorPanel& panel,
                                                                       const std::vector<std::string>& pool) {
    panel.set_recommendation_pool(pool);
}

void RoomOvalPointChildEditorPanelTestAccess::set_point_detail_values(
    RoomOvalPointChildEditorPanel& panel,
    const RoomOvalPointChildEditorPanel::PointDetailValues& values) {
    panel.set_point_detail_values(values);
}

void RoomOvalPointChildEditorPanelTestAccess::left_click_recommended(RoomOvalPointChildEditorPanel& panel,
                                                                      const std::string& tag) {
    panel.add_positive_tag(tag);
}

void RoomOvalPointChildEditorPanelTestAccess::right_click_recommended(RoomOvalPointChildEditorPanel& panel,
                                                                       const std::string& tag) {
    panel.add_negative_tag(tag);
}

void RoomOvalPointChildEditorPanelTestAccess::click_positive(RoomOvalPointChildEditorPanel& panel,
                                                              const std::string& tag) {
    panel.remove_positive_tag(tag);
}

void RoomOvalPointChildEditorPanelTestAccess::click_negative(RoomOvalPointChildEditorPanel& panel,
                                                              const std::string& tag) {
    panel.remove_negative_tag(tag);
}

std::vector<std::string> RoomOvalPointChildEditorPanelTestAccess::positive_tags(
    const RoomOvalPointChildEditorPanel& panel) {
    return sorted_set_values(panel.positive_tags_);
}

std::vector<std::string> RoomOvalPointChildEditorPanelTestAccess::negative_tags(
    const RoomOvalPointChildEditorPanel& panel) {
    return sorted_set_values(panel.negative_tags_);
}

const std::vector<std::string>& RoomOvalPointChildEditorPanelTestAccess::recommended_tags(
    const RoomOvalPointChildEditorPanel& panel) {
    return panel.recommended_tags_;
}
#endif
