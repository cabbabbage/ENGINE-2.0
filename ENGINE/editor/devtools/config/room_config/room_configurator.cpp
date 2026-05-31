#include "room_configurator.hpp"

#include "DockableCollapsible.hpp"
#include "core/AssetsManager.hpp"
#include "dm_styles.hpp"
#include "gameplay/map_generation/room.hpp"
#include "tag_editor_widget.hpp"
#include "tag_utils.hpp"
#include "utils/input.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"
#include "utils/display_color.hpp"
#include "utils/ranged_color.hpp"
#include "dev_color_picker.hpp"
#include "widgets.hpp"
#include "font_cache.hpp"
#include "dev_mode_color_utils.hpp"
#include "gameplay/spawn/spawn_group_codec.hpp"
#include "devtools/docked_panel_layout_policy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>
#include <utility>

namespace {
constexpr int kRoomConfigPanelMinWidth = 260;
constexpr int kDefaultRoomSize = 9;
constexpr int kMinRoomSize = 7;
constexpr int kMaxRoomSize = 20;
constexpr int kDefaultTrailSize = 5;
constexpr int kMinTrailSize = 5;
constexpr int kMaxTrailSize = 10;
constexpr int kSliderExpansionMargin = 64;
constexpr int kSliderExpansionFactor = 2;
constexpr double kDegreesFullCircle = 360.0;
constexpr double kTrailSectorDefaultDirectionDeg = 0.0;
constexpr int kTrailSectorDefaultWidthPercent = 100;
constexpr int kTrailSectorMinWidthPercent = 25;
constexpr int kTrailSectorMaxWidthPercent = 100;
constexpr int kMinCoarseness = 0;
constexpr int kMaxCoarseness = 4000;
constexpr int kMinCoarsenessRadius = 8;


const nlohmann::json& empty_object() {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    return kEmpty;
}

std::string lowercase_copy(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

std::optional<int> read_json_int(const nlohmann::json& obj, const std::string& key) {
    if (!obj.is_object() || !obj.contains(key)) {
        return std::nullopt;
    }
    const auto& value = obj[key];
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_float()) {
        return static_cast<int>(std::lround(value.get<double>()));
    }
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::optional<double> read_json_double(const nlohmann::json& obj, const std::string& key) {
    if (!obj.is_object() || !obj.contains(key)) {
        return std::nullopt;
    }
    const auto& value = obj[key];
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_number_integer()) {
        return static_cast<double>(value.get<int>());
    }
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
        }
    }
    return std::nullopt;
}

double normalize_angle_degrees(double value) {
    if (!std::isfinite(value)) {
        return kTrailSectorDefaultDirectionDeg;
    }
    double normalized = std::fmod(value, kDegreesFullCircle);
    if (normalized < 0.0) {
        normalized += kDegreesFullCircle;
    }
    if (normalized >= kDegreesFullCircle) {
        normalized -= kDegreesFullCircle;
    }
    return normalized;
}

double shortest_angular_distance_degrees(double a, double b) {
    const double na = normalize_angle_degrees(a);
    const double nb = normalize_angle_degrees(b);
    double delta = std::abs(na - nb);
    if (delta > 180.0) {
        delta = kDegreesFullCircle - delta;
    }
    return delta;
}

std::optional<bool> read_json_bool(const nlohmann::json& obj, const std::string& key) {
    if (!obj.is_object() || !obj.contains(key)) {
        return std::nullopt;
    }
    const auto& value = obj[key];
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int>() != 0;
    }
    if (value.is_string()) {
        const std::string raw = lowercase_copy(value.get<std::string>());
        if (raw == "true" || raw == "1" || raw == "yes" || raw == "on") {
            return true;
        }
        if (raw == "false" || raw == "0" || raw == "no" || raw == "off") {
            return false;
        }
    }
    return std::nullopt;
}

std::optional<std::string> read_json_string(const nlohmann::json& obj, const std::string& key) {
    if (!obj.is_object() || !obj.contains(key)) {
        return std::nullopt;
    }
    const auto& value = obj[key];
    if (!value.is_string()) {
        return std::nullopt;
    }
    return value.get<std::string>();
}

vibble::weighted_range::WeightedIntRange coarseness_range_from_legacy(int value) {
    const int clamped = std::clamp(value, kMinCoarseness, kMaxCoarseness);
    if (clamped <= 0) {
        return vibble::weighted_range::make_flat(0);
    }
    const int min_radius = std::max(kMinCoarsenessRadius, 12 + (clamped / 18));
    const int max_radius = std::max(min_radius, 36 + (clamped / 4));
    return vibble::weighted_range::make_legacy_uniform(min_radius, max_radius);
}

vibble::weighted_range::WeightedIntRange read_coarseness_range_field(const nlohmann::json& obj) {
    const auto fallback = vibble::weighted_range::make_flat(0);
    if (!obj.is_object() || !obj.contains("coarseness")) {
        return fallback;
    }
    const auto& value = obj["coarseness"];
    if (auto legacy = read_json_int(obj, "coarseness")) {
        return coarseness_range_from_legacy(*legacy);
    }
    return vibble::weighted_range::from_json(value, fallback);
}

bool append_unique(std::vector<std::string>& options, const std::string& value) {
    if (value.empty()) return false;
    if (std::find(options.begin(), options.end(), value) != options.end()) {
        return false;
    }
    options.push_back(value);
    return true;
}

std::vector<std::string> collect_room_tag_recommendations(const Assets* assets,
                                                          const std::string& current_room_name) {
    std::vector<std::string> recommendations;
    if (!assets) {
        return recommendations;
    }
    const nlohmann::json& map_info = assets->map_info_json();
    if (!map_info.is_object()) {
        return recommendations;
    }
    auto rooms_it = map_info.find("rooms_data");
    if (rooms_it == map_info.end() || !rooms_it->is_object()) {
        return recommendations;
    }

    auto add_tag = [&](const std::string& raw) {
        std::string normalized = tag_utils::normalize(raw);
        if (normalized.empty()) {
            return;
        }
        if (std::find(recommendations.begin(), recommendations.end(), normalized) == recommendations.end()) {
            recommendations.push_back(std::move(normalized));
        }
    };
    auto add_from_array = [&](const nlohmann::json& arr) {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& entry : arr) {
            if (entry.is_string()) {
                add_tag(entry.get<std::string>());
            }
        }
    };

    for (auto it = rooms_it->begin(); it != rooms_it->end(); ++it) {
        if (it.key() == current_room_name || !it.value().is_object()) {
            continue;
        }
        const auto& room_entry = it.value();
        if (room_entry.contains("room_tags")) {
            add_from_array(room_entry["room_tags"]);
        }
        if (room_entry.contains("tags")) {
            const auto& section = room_entry["tags"];
            if (section.is_array()) {
                add_from_array(section);
            } else if (section.is_object()) {
                if (section.contains("include")) add_from_array(section["include"]);
                if (section.contains("tags")) add_from_array(section["tags"]);
            }
        }
    }

    std::sort(recommendations.begin(), recommendations.end());
    return recommendations;
}

std::size_t hash_spawn_group_rows(const std::vector<RoomConfigurator::SpawnGroupListItem>& rows) {
    std::size_t seed = rows.size();
    auto combine = [&seed](const std::string& value) {
        seed ^= std::hash<std::string>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    };
    for (const auto& row : rows) {
        combine(row.spawn_id);
        combine(row.display_name);
        combine(row.icon_label);
    }
    return seed;
}

class SpawnGroupListWidget final : public Widget {
public:
    using ClickCallback = std::function<void(const std::string&)>;
    using DeleteCallback = std::function<bool(const std::string&)>;

    SpawnGroupListWidget(ClickCallback on_click,
                         ClickCallback on_double_click,
                         DeleteCallback on_delete)
        : on_click_(std::move(on_click)),
          on_double_click_(std::move(on_double_click)),
          on_delete_(std::move(on_delete)) {}

    void set_rows(std::vector<RoomConfigurator::SpawnGroupListItem> rows) {
        rows_ = std::move(rows);
        max_scroll_ = std::max(0, static_cast<int>(rows_.size()) * kRowHeight - content_height());
        scroll_px_ = std::clamp(scroll_px_, 0, max_scroll_);
    }

    void set_rect(const SDL_Rect& r) override {
        rect_ = r;
        max_scroll_ = std::max(0, static_cast<int>(rows_.size()) * kRowHeight - content_height());
        scroll_px_ = std::clamp(scroll_px_, 0, max_scroll_);
    }

    const SDL_Rect& rect() const override { return rect_; }

    int height_for_width(int) const override { return kPreferredHeight; }

    bool wants_full_row() const override { return true; }

    bool handle_event(const SDL_Event& e) override {
        if (rect_.w <= 0 || rect_.h <= 0) {
            return false;
        }
        if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            SDL_Point p = SDL_Point{
                static_cast<int>(std::lround(e.wheel.mouse_x)),
                static_cast<int>(std::lround(e.wheel.mouse_y))
            };
            if (!SDL_PointInRect(&p, &rect_)) {
                return false;
            }
            const int delta = -e.wheel.y * kWheelStepPx;
            if (delta != 0) {
                scroll_px_ = std::clamp(scroll_px_ + delta, 0, max_scroll_);
            }
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            if (!SDL_PointInRect(&p, &rect_)) {
                return false;
            }
            const int row_index = row_index_at_point(p);
            if (row_index < 0 || row_index >= static_cast<int>(rows_.size())) {
                return true;
            }
            const auto& row = rows_[static_cast<std::size_t>(row_index)];
            if (row.spawn_id.empty()) {
                return true;
            }
            if (point_in_delete_button(row_index, p)) {
                if (on_delete_) {
                    on_delete_(row.spawn_id);
                }
                return true;
            }
            const Uint32 now = SDL_GetTicks();
            const bool is_double_click = !last_clicked_id_.empty() &&
                                         last_clicked_id_ == row.spawn_id &&
                                         now >= last_click_time_ms_ &&
                                         (now - last_click_time_ms_) <= kDoubleClickMs;
            last_clicked_id_ = row.spawn_id;
            last_click_time_ms_ = now;
            if (is_double_click) {
                if (on_double_click_) {
                    on_double_click_(row.spawn_id);
                }
            } else if (on_click_) {
                on_click_(row.spawn_id);
            }
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            if (SDL_PointInRect(&p, &rect_)) {
                return true;
            }
        }
        return false;
    }

    void render(SDL_Renderer* renderer) const override {
        if (!renderer || rect_.w <= 0 || rect_.h <= 0) {
            return;
        }
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 120);
        sdl_render::FillRect(renderer, &rect_);
        SDL_Rect clip = rect_;
        SDL_SetRenderClipRect(renderer, &clip);

        const int first_row = std::max(0, scroll_px_ / kRowHeight);
        const int last_row = std::min(static_cast<int>(rows_.size()) - 1,
                                      (scroll_px_ + content_height()) / kRowHeight + 1);
        for (int i = first_row; i <= last_row; ++i) {
            if (i < 0 || i >= static_cast<int>(rows_.size())) {
                continue;
            }
            SDL_Rect row_rect = row_rect_for_index(i);
            const bool even = (i % 2) == 0;
            SDL_Color row_bg = even ? SDL_Color{34, 34, 34, 180} : SDL_Color{40, 40, 40, 180};
            SDL_SetRenderDrawColor(renderer, row_bg.r, row_bg.g, row_bg.b, row_bg.a);
            sdl_render::FillRect(renderer, &row_rect);

            SDL_SetRenderDrawColor(renderer, 66, 66, 66, 200);
            SDL_RenderLine(renderer, row_rect.x, row_rect.y + row_rect.h - 1, row_rect.x + row_rect.w, row_rect.y + row_rect.h - 1);

            const auto& row = rows_[static_cast<std::size_t>(i)];
            const std::string icon = row.icon_label.empty() ? "?" : row.icon_label;
            const std::string label = row.display_name.empty() ? row.spawn_id : row.display_name;
            render_text(renderer, icon, row_rect.x + kRowPadX, row_rect.y + 6, SDL_Color{170, 205, 255, 255}, 12);
            render_text(renderer, label, row_rect.x + kRowPadX + kIconWidth, row_rect.y + 6, DMStyles::Label().color, 13);

            SDL_Rect del = delete_button_rect_for_row(i);
            SDL_SetRenderDrawColor(renderer, 110, 48, 48, 220);
            sdl_render::FillRect(renderer, &del);
            SDL_SetRenderDrawColor(renderer, 168, 88, 88, 230);
            SDL_RenderLine(renderer, del.x, del.y, del.x + del.w - 1, del.y);
            SDL_RenderLine(renderer, del.x, del.y, del.x, del.y + del.h - 1);
            SDL_RenderLine(renderer, del.x + del.w - 1, del.y, del.x + del.w - 1, del.y + del.h - 1);
            SDL_RenderLine(renderer, del.x, del.y + del.h - 1, del.x + del.w - 1, del.y + del.h - 1);
            render_text(renderer, "X", del.x + 7, del.y + 3, SDL_Color{255, 220, 220, 255}, 12);
        }

        SDL_SetRenderClipRect(renderer, nullptr);
    }

private:
    static constexpr int kPreferredHeight = 220;
    static constexpr int kRowHeight = 30;
    static constexpr int kRowPadX = 8;
    static constexpr int kIconWidth = 28;
    static constexpr int kDeleteWidth = 24;
    static constexpr int kDeleteHeight = 20;
    static constexpr int kWheelStepPx = 22;
    static constexpr Uint32 kDoubleClickMs = 300;

    int content_height() const { return std::max(0, rect_.h); }

    SDL_Rect row_rect_for_index(int index) const {
        const int y = rect_.y + (index * kRowHeight) - scroll_px_;
        return SDL_Rect{rect_.x, y, rect_.w, kRowHeight};
    }

    SDL_Rect delete_button_rect_for_row(int index) const {
        SDL_Rect row = row_rect_for_index(index);
        return SDL_Rect{
            row.x + row.w - kDeleteWidth - 8,
            row.y + (row.h - kDeleteHeight) / 2,
            kDeleteWidth,
            kDeleteHeight
        };
    }

    bool point_in_delete_button(int row_index, SDL_Point p) const {
        SDL_Rect del = delete_button_rect_for_row(row_index);
        return SDL_PointInRect(&p, &del);
    }

    int row_index_at_point(SDL_Point p) const {
        const int local_y = p.y - rect_.y + scroll_px_;
        if (local_y < 0) return -1;
        return local_y / kRowHeight;
    }

    static void render_text(SDL_Renderer* renderer,
                            const std::string& text,
                            int x,
                            int y,
                            SDL_Color color,
                            int font_size) {
        if (text.empty()) return;
        TTF_Font* font = TTF_OpenFont(DMStyles::Label().font_path.c_str(), font_size);
        if (!font) return;
        SDL_Surface* surface = ttf_util::RenderTextBlended(font, text.c_str(), color);
        if (!surface) {
            TTF_CloseFont(font);
            return;
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture) {
            SDL_Rect dst{x, y, surface->w, surface->h};
            sdl_render::Texture(renderer, texture, nullptr, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_DestroySurface(surface);
        TTF_CloseFont(font);
    }

    SDL_Rect rect_{0, 0, 0, 0};
    std::vector<RoomConfigurator::SpawnGroupListItem> rows_{};
    ClickCallback on_click_{};
    ClickCallback on_double_click_{};
    DeleteCallback on_delete_{};
    std::string last_clicked_id_{};
    Uint32 last_click_time_ms_ = 0;
    int scroll_px_ = 0;
    int max_scroll_ = 0;
};

class TrailConnectionSectorWidget final : public Widget {
public:
    using ChangeCallback = std::function<void(double direction_deg, int width_percent)>;

    TrailConnectionSectorWidget(double direction_deg, int width_percent, ChangeCallback on_change)
        : direction_deg_(normalize_angle_degrees(direction_deg)),
          width_percent_(std::clamp(width_percent, kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent)),
          on_change_(std::move(on_change)) {}

    void set_state(double direction_deg, int width_percent) {
        if (drag_mode_ != DragMode::None) {
            return;
        }
        direction_deg_ = normalize_angle_degrees(direction_deg);
        width_percent_ = std::clamp(width_percent, kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent);
    }

    void set_rect(const SDL_Rect& r) override {
        rect_ = r;
    }

    const SDL_Rect& rect() const override {
        return rect_;
    }

    int height_for_width(int w) const override {
        const int clamped = std::clamp(w, 180, 420);
        return std::max(200, clamped - 40);
    }

    bool wants_full_row() const override { return true; }

    bool handle_event(const SDL_Event& e) override {
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            if (!SDL_PointInRect(&p, &rect_)) {
                return false;
            }
            const CircleLayout layout = compute_layout();
            const float dx = static_cast<float>(p.x) - layout.center.x;
            const float dy = static_cast<float>(p.y) - layout.center.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > layout.radius + 10.0f) {
                return false;
            }
            const double angle = angle_from_point(p, layout.center);
            const double span_deg = current_span_degrees();
            const double start_deg = normalize_angle_degrees(direction_deg_ - span_deg * 0.5);
            const double end_deg = normalize_angle_degrees(direction_deg_ + span_deg * 0.5);
            const bool near_perimeter = std::abs(dist - layout.radius) <= 16.0f;
            const bool near_start = shortest_angular_distance_degrees(angle, start_deg) <= 10.0;
            const bool near_end = shortest_angular_distance_degrees(angle, end_deg) <= 10.0;
            drag_mode_ = (near_perimeter && (near_start || near_end)) ? DragMode::Width : DragMode::Direction;
            update_from_angle(angle);
            return true;
        }

        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            if (drag_mode_ == DragMode::None) {
                return false;
            }
            SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
            const CircleLayout layout = compute_layout();
            const double angle = angle_from_point(p, layout.center);
            update_from_angle(angle);
            return true;
        }

        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            if (drag_mode_ == DragMode::None) {
                return false;
            }
            drag_mode_ = DragMode::None;
            return true;
        }

        return false;
    }

    void render(SDL_Renderer* renderer) const override {
        if (!renderer || rect_.w <= 0 || rect_.h <= 0) {
            return;
        }

        const SDL_Color panel_bg = DMStyles::PanelBG();
        const Uint8 panel_alpha = static_cast<Uint8>(std::max(80, static_cast<int>(panel_bg.a)));
        SDL_SetRenderDrawColor(renderer, panel_bg.r, panel_bg.g, panel_bg.b, panel_alpha);
        sdl_render::FillRect(renderer, &rect_);

        const CircleLayout layout = compute_layout();
        const double span_deg = current_span_degrees();
        const double start_deg = normalize_angle_degrees(direction_deg_ - span_deg * 0.5);
        const double end_deg = normalize_angle_degrees(direction_deg_ + span_deg * 0.5);

        // Blocked region tint
        render_wedge(renderer,
                     layout,
                     normalize_angle_degrees(end_deg),
                     std::max(0.0, kDegreesFullCircle - span_deg),
                     SDL_Color{45, 45, 45, 150});
        // Allowed sector tint
        render_wedge(renderer,
                     layout,
                     start_deg,
                     span_deg,
                     SDL_Color{70, 190, 130, 170});

        // Circle outline
        std::vector<SDL_Point> outline;
        constexpr int kSegments = 96;
        outline.reserve(kSegments + 1);
        for (int i = 0; i <= kSegments; ++i) {
            const double t = kDegreesFullCircle * (static_cast<double>(i) / kSegments);
            outline.push_back(to_point_for_angle(layout, t, layout.radius));
        }
        const SDL_Color border = DMStyles::Border();
        SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, 220);
        if (!outline.empty()) {
            sdl_render::Lines(renderer, outline.data(), static_cast<int>(outline.size()));
        }

        const SDL_Point center_pt{static_cast<int>(std::lround(layout.center.x)), static_cast<int>(std::lround(layout.center.y))};
        const SDL_Point dir_tip = to_point_for_angle(layout, direction_deg_, layout.radius + 10.0f);
        SDL_SetRenderDrawColor(renderer, 220, 235, 245, 235);
        SDL_RenderLine(renderer, center_pt.x, center_pt.y, dir_tip.x, dir_tip.y);

        const SDL_Point start_handle = to_point_for_angle(layout, start_deg, layout.radius);
        const SDL_Point end_handle = to_point_for_angle(layout, end_deg, layout.radius);
        draw_handle(renderer, start_handle, drag_mode_ == DragMode::Width);
        draw_handle(renderer, end_handle, drag_mode_ == DragMode::Width);
        draw_handle(renderer, dir_tip, drag_mode_ == DragMode::Direction);

        const std::string readout =
            "Direction: " + std::to_string(static_cast<int>(std::lround(direction_deg_))) +
            " deg   Width: " + std::to_string(width_percent_) + "%";
        draw_centered_text(renderer, readout, rect_.x + rect_.w / 2, rect_.y + rect_.h - 18);
    }

private:
    struct CircleLayout {
        SDL_FPoint center{0.0f, 0.0f};
        float radius = 0.0f;
    };

    enum class DragMode {
        None,
        Direction,
        Width,
    };

    CircleLayout compute_layout() const {
        CircleLayout layout;
        const int pad = DMSpacing::panel_padding();
        const int text_reserve = 28;
        const int inner_w = std::max(1, rect_.w - pad * 2);
        const int inner_h = std::max(1, rect_.h - pad * 2 - text_reserve);
        const int diameter = std::max(24, std::min(inner_w, inner_h));
        layout.radius = static_cast<float>(diameter) * 0.5f - 6.0f;
        layout.center = SDL_FPoint{
            static_cast<float>(rect_.x + rect_.w / 2),
            static_cast<float>(rect_.y + pad + inner_h / 2),
        };
        return layout;
    }

    double current_span_degrees() const {
        return (kDegreesFullCircle * static_cast<double>(width_percent_)) / 100.0;
    }

    static SDL_Point to_point_for_angle(const CircleLayout& layout, double angle_deg, float radius) {
        const double normalized = normalize_angle_degrees(angle_deg);
        const double rad = normalized * (3.14159265358979323846 / 180.0);
        const double x = static_cast<double>(layout.center.x) + std::sin(rad) * static_cast<double>(radius);
        const double y = static_cast<double>(layout.center.y) - std::cos(rad) * static_cast<double>(radius);
        return SDL_Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
    }

    static double angle_from_point(const SDL_Point& point, const SDL_FPoint& center) {
        const double dx = static_cast<double>(point.x) - static_cast<double>(center.x);
        const double dy = static_cast<double>(point.y) - static_cast<double>(center.y);
        const double angle_rad = std::atan2(dx, -dy);
        return normalize_angle_degrees(angle_rad * (180.0 / 3.14159265358979323846));
    }

    void update_from_angle(double angle_deg) {
        bool changed = false;
        if (drag_mode_ == DragMode::Direction) {
            const double next = normalize_angle_degrees(angle_deg);
            if (std::abs(next - direction_deg_) > 1e-6) {
                direction_deg_ = next;
                changed = true;
            }
        } else if (drag_mode_ == DragMode::Width) {
            const double delta = shortest_angular_distance_degrees(angle_deg, direction_deg_);
            const double span = std::clamp(delta * 2.0, 90.0, 360.0);
            int next_width = static_cast<int>(std::lround((span / kDegreesFullCircle) * 100.0));
            next_width = std::clamp(next_width, kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent);
            if (next_width != width_percent_) {
                width_percent_ = next_width;
                changed = true;
            }
        }

        if (changed && on_change_) {
            on_change_(direction_deg_, width_percent_);
        }
    }

    static void draw_handle(SDL_Renderer* renderer, const SDL_Point& p, bool highlighted) {
        const SDL_Color color = highlighted ? SDL_Color{255, 220, 120, 255} : SDL_Color{230, 230, 230, 220};
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_FRect r{
            static_cast<float>(p.x - 4),
            static_cast<float>(p.y - 4),
            8.0f,
            8.0f,
        };
        sdl_render::FillRect(renderer, &r);
    }

    static void draw_centered_text(SDL_Renderer* renderer, const std::string& text, int x, int y) {
        const DMLabelStyle label = DMStyles::Label();
        TTF_Font* font = TTF_OpenFont(label.font_path.c_str(), label.font_size);
        if (!font) {
            return;
        }
        SDL_Surface* surface = ttf_util::RenderTextBlended(font, text, label.color);
        if (!surface) {
            TTF_CloseFont(font);
            return;
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture) {
            SDL_Rect dst{x - surface->w / 2, y - surface->h / 2, surface->w, surface->h};
            sdl_render::Texture(renderer, texture, nullptr, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_DestroySurface(surface);
        TTF_CloseFont(font);
    }

    static void render_wedge(SDL_Renderer* renderer,
                             const CircleLayout& layout,
                             double start_deg,
                             double sweep_deg,
                             SDL_Color color) {
        if (sweep_deg <= 0.0) {
            return;
        }
        const auto to_color = [](SDL_Color c) {
            return SDL_FColor{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
        };

        const int segments = std::max(8, static_cast<int>(std::ceil(std::abs(sweep_deg) / 6.0)));
        std::vector<SDL_Vertex> verts;
        verts.reserve(static_cast<size_t>(segments) + 2);
        SDL_Vertex center{};
        center.position = layout.center;
        center.color = to_color(color);
        verts.push_back(center);
        for (int i = 0; i <= segments; ++i) {
            const double t = normalize_angle_degrees(start_deg + sweep_deg * (static_cast<double>(i) / segments));
            const SDL_Point p = to_point_for_angle(layout, t, layout.radius);
            SDL_Vertex v{};
            v.position = SDL_FPoint{static_cast<float>(p.x), static_cast<float>(p.y)};
            v.color = to_color(color);
            verts.push_back(v);
        }
        std::vector<int> idx;
        idx.reserve(static_cast<size_t>(segments) * 3);
        for (int i = 1; i <= segments; ++i) {
            idx.push_back(0);
            idx.push_back(i);
            idx.push_back(i + 1);
        }
        SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()), idx.data(), static_cast<int>(idx.size()));
    }

    SDL_Rect rect_{0, 0, 0, 0};
    double direction_deg_ = kTrailSectorDefaultDirectionDeg;
    int width_percent_ = kTrailSectorDefaultWidthPercent;
    ChangeCallback on_change_{};
    DragMode drag_mode_ = DragMode::None;
};

} // namespace

struct RoomConfigurator::State {
    std::string name;
    int size = kDefaultRoomSize;
    double trail_connection_direction_deg = kTrailSectorDefaultDirectionDeg;
    int trail_connection_width_percent = kTrailSectorDefaultWidthPercent;
    bool is_boss = false;
    bool has_boss_field = false;
    bool inherits_assets = false;
    bool inherit_map_floor_color = true;
    SDL_Color room_floor_color{0, 0, 0, 255};
    vibble::weighted_range::WeightedIntRange coarseness = vibble::weighted_range::make_flat(0);

    bool ensure_valid(bool allow_height, bool enforce_dimensions = true) {
        (void)enforce_dimensions;
        const bool is_trail_context = !allow_height;
        bool mutated = false;
        if (is_trail_context) {
            const int normalized = (size < kMinTrailSize || size > kMaxTrailSize)
                ? kDefaultTrailSize
                : std::clamp(size, kMinTrailSize, kMaxTrailSize);
            if (normalized != size) {
                size = normalized;
                mutated = true;
            }
        } else {
            const int clamped_size = std::clamp(size, kMinRoomSize, kMaxRoomSize);
            if (clamped_size != size) {
                size = clamped_size;
                mutated = true;
            }
        }

        const double normalized_direction = normalize_angle_degrees(trail_connection_direction_deg);
        if (std::abs(normalized_direction - trail_connection_direction_deg) > 1e-6) {
            trail_connection_direction_deg = normalized_direction;
            mutated = true;
        }
        const int clamped_width =
            std::clamp(trail_connection_width_percent, kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent);
        if (clamped_width != trail_connection_width_percent) {
            trail_connection_width_percent = clamped_width;
            mutated = true;
        }
        const auto coarseness_json = vibble::weighted_range::to_json(coarseness);
        coarseness = vibble::weighted_range::from_json(coarseness_json, vibble::weighted_range::make_flat(0));
        if (vibble::weighted_range::to_json(coarseness) != coarseness_json) {
            mutated = true;
        }
        return mutated;
    }

    void load_from_json(const nlohmann::json& data,
                        const std::vector<std::string>& geometry_options,
                        bool allow_height) {
        (void)geometry_options;
        const bool is_trail_context = !allow_height;
        const nlohmann::json& src = data.is_object() ? data : empty_object();
        if (auto value = read_json_string(src, "name")) {
            name = *value;
        } else if (auto value = read_json_string(src, "room_name")) {
            name = *value;
        } else {
            name.clear();
        }

        size = is_trail_context ? kDefaultTrailSize : kDefaultRoomSize;
        if (auto value = read_json_int(src, "size")) {
            size = *value;
        }

        has_boss_field = src.is_object() && src.contains("is_boss");
        if (auto value = read_json_bool(src, "is_boss")) {
            is_boss = *value;
        } else {
            is_boss = false;
        }
        if (auto value = read_json_bool(src, "inherits_live_dynamic_assets")) {
            inherits_assets = *value;
        } else if (auto legacy_value = read_json_bool(src, "inherits_map_assets")) {
            inherits_assets = *legacy_value;
        } else {
            inherits_assets = false;
        }
        if (auto value = read_json_bool(src, "inherit_map_floor_color")) {
            inherit_map_floor_color = *value;
        } else {
            inherit_map_floor_color = true;
        }
        if (src.contains("room_floor_color")) {
            if (auto parsed = utils::color::color_from_json(src["room_floor_color"])) {
                room_floor_color = *parsed;
                room_floor_color.a = 255;
            }
        } else {
            room_floor_color = SDL_Color{0, 0, 0, 255};
        }
        coarseness = read_coarseness_range_field(src);
        trail_connection_direction_deg = kTrailSectorDefaultDirectionDeg;
        trail_connection_width_percent = kTrailSectorDefaultWidthPercent;
        if (src.contains("trail_connection_sector") && src["trail_connection_sector"].is_object()) {
            const auto& sector = src["trail_connection_sector"];
            if (auto value = read_json_double(sector, "direction_deg")) {
                trail_connection_direction_deg = *value;
            }
            if (auto value = read_json_int(sector, "width_percent")) {
                trail_connection_width_percent = *value;
            }
        }

        ensure_valid(allow_height);
    }

    void apply_to_json(nlohmann::json& dest,
                       bool allow_height,
                       bool include_camera = true,
                       bool include_trail_connection_sector = true,
                       bool include_boss = true) const {
        const bool is_trail_context = !allow_height;
        if (!dest.is_object()) dest = nlohmann::json::object();
        dest["name"] = name;
        if (is_trail_context) {
            dest["size"] = (size < kMinTrailSize || size > kMaxTrailSize)
                ? kDefaultTrailSize
                : std::clamp(size, kMinTrailSize, kMaxTrailSize);
        } else {
            dest["size"] = std::clamp(size, kMinRoomSize, kMaxRoomSize);
        }
        if (include_boss) {
            dest["is_boss"] = is_boss;
        } else {
            dest.erase("is_boss");
        }
        dest["inherits_live_dynamic_assets"] = inherits_assets;
        dest["inherit_map_floor_color"] = inherit_map_floor_color;
        dest["room_floor_color"] = nlohmann::json::array(
            {static_cast<int>(room_floor_color.r), static_cast<int>(room_floor_color.g), static_cast<int>(room_floor_color.b)});
        dest["coarseness"] = vibble::weighted_range::to_json(coarseness);
        (void)include_camera;

        dest.erase("radius");
        dest.erase("min_radius");
        dest.erase("max_radius");
        dest.erase("min_width");
        dest.erase("max_width");
        dest.erase("min_height");
        dest.erase("max_height");
        dest.erase("width");
        dest.erase("height");
        dest.erase("geometry");
        dest.erase("edge_smoothness");
        dest.erase("curvyness");
        dest.erase("curviness");
        dest.erase("edge_detail_candidates");
        dest.erase("inherits_map_assets");
        if (!allow_height) {
            dest.erase("inherit_map_floor_color");
            dest.erase("inherits_live_dynamic_assets");
            dest.erase("room_floor_color");
        }
        if (include_trail_connection_sector) {
            dest["trail_connection_sector"] = nlohmann::json::object({
                {"direction_deg", normalize_angle_degrees(trail_connection_direction_deg)},
                {"width_percent",
                 std::clamp(trail_connection_width_percent, kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent)},
            });
        } else {
            dest.erase("trail_connection_sector");
        }
    }
};

std::vector<std::string> sorted_strings(const std::vector<std::string>& values) {
    std::vector<std::string> copy = values;
    std::sort(copy.begin(), copy.end());
    return copy;
}

nlohmann::json sorted_strings_json(const std::vector<std::string>& values) {
    return sorted_strings(values);
}

nlohmann::json build_metadata_snapshot_json(const nlohmann::json& canonical_metadata,
                                            const std::vector<std::string>& tags,
                                            const std::vector<std::string>& anti_tags,
                                            bool include_tags) {
    nlohmann::json snapshot = nlohmann::json::object();
    if (canonical_metadata.is_object()) {
        snapshot = canonical_metadata;
    }
    if (include_tags) {
        snapshot["room_tags"] = sorted_strings_json(tags);
        snapshot["anti_tags"] = sorted_strings_json(anti_tags);
    }
    return snapshot;
}

std::size_t hash_metadata_snapshot(const nlohmann::json& snapshot) {
    return std::hash<std::string>{}(snapshot.dump());
}

nlohmann::json collect_owned_metadata_fields_raw(const nlohmann::json& source,
                                                 bool include_tags,
                                                 bool allow_height,
                                                 bool include_trail_connection_sector) {
    nlohmann::json raw = nlohmann::json::object();
    if (!source.is_object()) {
        return raw;
    }
    auto copy_field = [&source, &raw](const char* key) {
        auto it = source.find(key);
        if (it != source.end()) {
            raw[key] = *it;
        }
    };
    copy_field("name");
    copy_field("room_name");
    (void)allow_height;
    copy_field("size");
    copy_field("coarseness");
    if (include_trail_connection_sector || source.contains("trail_connection_sector")) {
        copy_field("trail_connection_sector");
    }
    copy_field("is_boss");
    copy_field("inherits_live_dynamic_assets");
    copy_field("inherits_map_assets");
    copy_field("inherit_map_floor_color");
    copy_field("room_floor_color");
    if (include_tags) {
        copy_field("room_tags");
        copy_field("tags");
        copy_field("anti_tags");
    }
    return raw;
}

RoomConfigurator::RoomConfigurator() {
    state_ = std::make_unique<State>();
    default_container_ = std::make_unique<SlidingWindowContainer>();
    container_ = default_container_.get();
    configure_container(*container_);
}

RoomConfigurator::~RoomConfigurator() {
    if (container_ && container_ != default_container_.get()) {
        clear_container_callbacks(*container_);
    }
}

void RoomConfigurator::set_room_metadata_only_mode(bool enabled) {
    if (room_metadata_only_mode_ == enabled) {
        return;
    }
    room_metadata_only_mode_ = enabled;
    request_rebuild();
}

void RoomConfigurator::set_spawn_groups_provider(std::function<std::vector<SpawnGroupListItem>()> provider) {
    spawn_groups_provider_ = std::move(provider);
    spawn_group_rows_hash_ = 0;
    request_rebuild();
}

void RoomConfigurator::set_on_spawn_group_click(std::function<void(const std::string&)> cb) {
    on_spawn_group_click_ = std::move(cb);
    request_rebuild();
}

void RoomConfigurator::set_on_spawn_group_double_click(std::function<void(const std::string&)> cb) {
    on_spawn_group_double_click_ = std::move(cb);
    request_rebuild();
}

void RoomConfigurator::set_on_spawn_group_delete(std::function<bool(const std::string&)> cb) {
    on_spawn_group_delete_ = std::move(cb);
    request_rebuild();
}

void RoomConfigurator::set_manifest_store(devmode::core::ManifestStore* store) {
    manifest_store_ = store;
}

void RoomConfigurator::set_assets(Assets* assets) {
    assets_ = assets;
}

void RoomConfigurator::set_bounds(const SDL_Rect& bounds) {
    bounds_override_ = bounds;
    has_bounds_override_ = bounds.w > 0 && bounds.h > 0;
    SDL_Rect applied = bounds;
    if (has_bounds_override_) {
        applied.w = std::max(0, applied.w);
        applied.h = std::max(0, applied.h);
        const int padding = DMSpacing::panel_padding();
        int min_panel_w = kRoomConfigPanelMinWidth + padding * 2;
        if (applied.w > 0) {
            applied.w = std::max(min_panel_w, applied.w);
        }
        if (container_) {
            container_->set_panel_bounds_override(applied);
        }
    } else {
        if (container_) {
            container_->clear_panel_bounds_override();
        }
    }
    if (!has_bounds_override_) {
        applied = work_area_;
    }
    ensure_base_panels();
    if (geometry_panel_) geometry_panel_->set_work_area(applied);
    if (tags_panel_) tags_panel_->set_work_area(applied);
    if (types_panel_) types_panel_->set_work_area(applied);
    if (spawn_groups_panel_) spawn_groups_panel_->set_work_area(applied);
    request_container_layout();
}

void RoomConfigurator::set_work_area(const SDL_Rect& bounds) {
    work_area_ = bounds;
    ensure_base_panels();
    if (geometry_panel_) geometry_panel_->set_work_area(bounds);
    if (tags_panel_) tags_panel_->set_work_area(bounds);
    if (types_panel_) types_panel_->set_work_area(bounds);
    if (spawn_groups_panel_) spawn_groups_panel_->set_work_area(bounds);
    request_container_layout();
}

void RoomConfigurator::set_show_header(bool show) {
    show_header_ = show;
    if (container_) {
        container_->set_header_visible(show_header_);
    }
}

void RoomConfigurator::set_on_close(std::function<void()> cb) { on_close_ = std::move(cb); }

void RoomConfigurator::set_header_visibility_controller(std::function<void(bool)> cb) {
    header_visibility_controller_ = std::move(cb);
    if (container_) {
        container_->set_header_visibility_controller(header_visibility_controller_);
    }
}

void RoomConfigurator::set_blocks_editor_interactions(bool block) {
    if (blocks_editor_interactions_ == block) {
        return;
    }
    blocks_editor_interactions_ = block;
    if (container_) {
        container_->set_blocks_editor_interactions(blocks_editor_interactions_);
    }
    if (default_container_ && default_container_.get() != container_) {
        default_container_->set_blocks_editor_interactions(blocks_editor_interactions_);
    }
}

void RoomConfigurator::reset_scroll() {
    if (container_) {
        container_->reset_scroll();
    }
}

void RoomConfigurator::attach_container(SlidingWindowContainer* container) {
    if (container == container_) {
        return;
    }
    if (!container) {
        detach_container();
        return;
    }
    if (container_ && container_ != default_container_.get()) {
        clear_container_callbacks(*container_);
    }
    container_ = container;
    configure_container(*container_);
    if (has_bounds_override_) {
        set_bounds(bounds_override_);
    } else {
        request_container_layout();
    }
}

void RoomConfigurator::detach_container() {
    SlidingWindowContainer* previous = container_;
    if (previous && previous != default_container_.get()) {
        clear_container_callbacks(*previous);
    }
    if (!default_container_) {
        default_container_ = std::make_unique<SlidingWindowContainer>();
    }
    container_ = default_container_.get();
    if (container_) {
        configure_container(*container_);
        if (has_bounds_override_) {
            set_bounds(bounds_override_);
        } else {
            request_container_layout();
        }
    }
}

SlidingWindowContainer* RoomConfigurator::container() { return container_; }

const SlidingWindowContainer* RoomConfigurator::container() const { return container_; }

void RoomConfigurator::configure_container(SlidingWindowContainer& container) {
    container.set_docked_layout_policy(devmode::docked_panels::DockedPanelLayoutPolicy::FullHeightDefault);
    container.set_header_text_provider([this]() { return this->current_header_text(); });
    const bool can_generate_live_room = (room_ != nullptr) && !is_trail_context_ && !room_metadata_only_mode_;
    if (can_generate_live_room && on_generate_room_) {
        container.set_header_navigation_button(
            "Generate",
            [this]() {
                if (!on_generate_room_) {
                    return;
                }
                std::string template_key;
                if (room_ && !room_->room_name.empty()) {
                    template_key = room_->room_name;
                } else if (state_ && !state_->name.empty()) {
                    template_key = state_->name;
                }
                if (!template_key.empty()) {
                    on_generate_room_(template_key);
                }
            },
            &DMStyles::CreateButton());
        container.set_header_navigation_alignment_right(true);
    } else {
        container.clear_header_navigation_button();
    }
    container.set_on_close([this]() { handle_container_closed(); });
    container.set_layout_function([this](const SlidingWindowContainer::LayoutContext& ctx) {
        return this->layout_content(ctx);
    });
    container.set_render_function([this](SDL_Renderer* renderer) {
        for (size_t i = 0; i < ordered_base_panels_.size(); ++i) {
            auto* panel = ordered_base_panels_[i];
            if (panel && panel->is_visible()) {
                SDL_Rect bounds = (i < ordered_panel_bounds_.size()) ? ordered_panel_bounds_[i] : panel->rect();
                panel->render_embedded(renderer, bounds, last_screen_w_, last_screen_h_);
            }
        }
    });
    container.set_event_function([this](const SDL_Event& e) {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
            this->close();
            return true;
        }
        if (handle_panel_focus_event(e)) {
            return true;
        }
        const bool pointer_event =
            (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION);
        const bool wheel_event = (e.type == SDL_EVENT_MOUSE_WHEEL);
        auto dispatch_to_panel = [this, &e](DockableCollapsible* panel) -> bool {
            if (!panel || !panel->is_visible()) {
                return false;
            }
            DockableCollapsible* panel_before_event = panel;
            if (!panel_before_event->handle_event(e)) {
                return false;
            }
            request_container_layout();
            const auto panel_still_active = [this](DockableCollapsible* candidate) {
                if (!candidate) {
                    return false;
                }
                for (auto* active_panel : ordered_base_panels_) {
                    if (active_panel == candidate) {
                        return true;
                    }
                }
                return false;
            };
            if (panel_still_active(panel_before_event)) {
                auto it = base_panel_keys_.find(panel_before_event);
                if (it != base_panel_keys_.end()) {
                    set_base_panel_expanded(it->second, panel_before_event->is_expanded());
                }
            }
            return true;
        };

        DockableCollapsible* pointer_panel = nullptr;
        if (pointer_event) {
            SDL_Point pointer{};
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                pointer = sdl_mouse_util::MotionPoint(e.motion);
            } else {
                pointer = sdl_mouse_util::ButtonPoint(e.button);
            }
            pointer_panel = panel_at_point(pointer);
        } else if (wheel_event) {
            SDL_Point pointer{};
            sdl_mouse_util::GetMouseState(&pointer.x, &pointer.y);
            pointer_panel = panel_at_point(pointer);
        }

        // Always route pointer/wheel input to the panel under the cursor first.
        if (dispatch_to_panel(pointer_panel)) {
            return true;
        }

        // If we cannot resolve a pointer panel (e.g. during transient relayout),
        // fall back to the focused panel before broad panel dispatch.
        bool focused_dispatched = false;
        if ((pointer_event || wheel_event) && !pointer_panel) {
            focused_dispatched = dispatch_to_panel(focused_panel_);
            if (focused_dispatched) {
                return true;
            }
        }

        // Non-pointer interactions still prefer focused panel semantics.
        if (!pointer_event && !wheel_event && dispatch_to_panel(focused_panel_)) {
            return true;
        }

        for (auto* panel : ordered_base_panels_) {
            if (panel == pointer_panel) {
                continue;
            }
            if (focused_dispatched && panel == focused_panel_) {
                continue;
            }
            if (dispatch_to_panel(panel)) {
                return true;
            }
        }
        return false;
    });
    container.set_update_function([this](const Input& input, int screen_w, int screen_h) {
        for (auto* panel : ordered_base_panels_) {
            if (panel) panel->update(input, screen_w, screen_h);
        }
    });
    container.set_cancel_function([this]() {
        for (auto* panel : ordered_base_panels_) {
            if (panel) {
                panel->cancel_child_interactions();
            }
        }
        focused_panel_ = nullptr;
    });
    container.set_blocks_editor_interactions(blocks_editor_interactions_);
    container.set_scrollbar_visible(true);
    container.set_content_clip_enabled(true);
    container.set_header_visible(show_header_);
    container.set_header_visibility_controller(header_visibility_controller_);
    if (!has_bounds_override_) {
        container.clear_panel_bounds_override();
    }
}

void RoomConfigurator::clear_container_callbacks(SlidingWindowContainer& container) {
    container.set_header_text_provider({});
    container.set_on_close({});
    container.set_layout_function({});
    container.set_render_function({});
    container.set_event_function({});
    container.set_cancel_function({});
    container.set_update_function({});
    container.set_header_visibility_controller({});
    container.set_blocks_editor_interactions(false);
    container.clear_panel_bounds_override();
}

SDL_Rect RoomConfigurator::clamp_to_work_area(const SDL_Rect& bounds) const {
    if (work_area_.w <= 0 || work_area_.h <= 0) {
        return bounds;
    }
    SDL_Rect result = bounds;
    result.w = std::max(1, std::min(result.w, work_area_.w));
    result.h = std::max(1, std::min(result.h, work_area_.h));
    int min_x = work_area_.x;
    int max_x = work_area_.x + work_area_.w - result.w;
    int min_y = work_area_.y;
    int max_y = work_area_.y + work_area_.h - result.h;
    if (max_x < min_x) max_x = min_x;
    if (max_y < min_y) max_y = min_y;
    result.x = std::clamp(result.x, min_x, max_x);
    result.y = std::clamp(result.y, min_y, max_y);
    return result;
}

void RoomConfigurator::ensure_base_panels() {
    auto ensure_panel = [&](std::unique_ptr<DockableCollapsible>& panel,
                            const std::string& key,
                            const std::string& title) {
        const bool created = !panel;
        if (!panel) {
            panel = std::make_unique<DockableCollapsible>(title, false);
            panel->set_floatable(false);
            panel->set_show_header(true);
            panel->set_close_button_enabled(false);
            panel->set_scroll_enabled(false);
            panel->set_row_gap(DMSpacing::item_gap());
            panel->set_col_gap(DMSpacing::item_gap());
            panel->set_padding(DMSpacing::panel_padding());
            panel->reset_scroll();
            panel->set_visible(true);
            panel->force_pointer_ready();
            panel->set_embedded_focus_state(false);
            panel->set_embedded_interaction_enabled(true);
        }
        base_panel_keys_[panel.get()] = key;
        if (created && !base_panel_expanded_state_.count(key)) {
            base_panel_expanded_state_[key] = false;
        }
        bool expanded = base_panel_expanded(key);
        if (panel->is_expanded() != expanded) {
            panel->set_expanded(expanded);
        }
    };

    const std::string geometry_title = is_trail_context_ ? "Trail Size" : "Room Size";
    const std::string tags_title = is_trail_context_ ? "Trail Tags" : "Room Tags";
    const std::string types_title = is_trail_context_ ? "Trail Types" : "Room Types";
    const std::string spawn_groups_title = "Spawn Groups";

    ensure_panel(geometry_panel_, "geometry", geometry_title);
    if (!room_metadata_only_mode_) {
        ensure_panel(tags_panel_, "tags", tags_title);
    } else if (tags_panel_) {
        tags_panel_->set_visible(false);
    }
    ensure_panel(types_panel_, "types", types_title);
    ensure_panel(spawn_groups_panel_, "spawn_groups", spawn_groups_title);
}

void RoomConfigurator::refresh_base_panel_rows() {
    ordered_base_panels_.clear();
    ordered_panel_bounds_.clear();

    if (geometry_panel_) {
        DockableCollapsible::Rows rows;
        if (name_widget_) rows.push_back({name_widget_.get()});
        if (size_widget_) rows.push_back({size_widget_.get()});
        if (coarseness_widget_) rows.push_back({coarseness_widget_.get()});
        if (trail_connection_sector_widget_) rows.push_back({trail_connection_sector_widget_.get()});
        if (sector_direction_widget_) rows.push_back({sector_direction_widget_.get()});
        if (sector_width_widget_) rows.push_back({sector_width_widget_.get()});
        if (sector_reset_widget_) rows.push_back({sector_reset_widget_.get()});
        geometry_panel_->set_rows(rows);
        geometry_panel_->set_visible(!rows.empty());
        if (!rows.empty()) {
            ordered_base_panels_.push_back(geometry_panel_.get());
        }
    }

    if (!room_metadata_only_mode_ && tags_panel_ && tag_editor_) {
        DockableCollapsible::Rows rows;
        rows.push_back({tag_editor_.get()});
        tags_panel_->set_rows(rows);
        tags_panel_->set_visible(!rows.empty());
        if (!rows.empty()) {
            ordered_base_panels_.push_back(tags_panel_.get());
        }
    } else if (tags_panel_) {
        tags_panel_->set_rows({});
        tags_panel_->set_visible(false);
    }

    if (types_panel_) {
        DockableCollapsible::Rows rows;
        DockableCollapsible::Row toggles;
        if (boss_widget_) toggles.push_back(boss_widget_.get());
        if (inherit_widget_) toggles.push_back(inherit_widget_.get());
        if (inherit_floor_color_widget_) toggles.push_back(inherit_floor_color_widget_.get());
        if (!toggles.empty()) {
            rows.push_back(std::move(toggles));
        }
        if (state_ && !state_->inherit_map_floor_color && room_floor_color_widget_) {
            rows.push_back({room_floor_color_widget_.get()});
        }
        types_panel_->set_rows(rows);
        types_panel_->set_visible(!rows.empty());
        if (!rows.empty()) {
            ordered_base_panels_.push_back(types_panel_.get());
        }
    }

    if (spawn_groups_panel_ && spawn_group_list_widget_) {
        DockableCollapsible::Rows rows;
        rows.push_back({spawn_group_list_widget_.get()});
        spawn_groups_panel_->set_rows(rows);
        spawn_groups_panel_->set_visible(true);
        ordered_base_panels_.push_back(spawn_groups_panel_.get());
    } else if (spawn_groups_panel_) {
        spawn_groups_panel_->set_rows({});
        spawn_groups_panel_->set_visible(false);
    }
    apply_panel_focus_states();
}

void RoomConfigurator::refresh_spawn_group_rows_if_needed() {
    std::vector<SpawnGroupListItem> rows;
    if (spawn_groups_provider_) {
        rows = spawn_groups_provider_();
    } else if (room_) {
        auto& root = room_->assets_data();
        auto it = root.find("spawn_groups");
        if (it != root.end() && it->is_array()) {
            rows.reserve(it->size());
            for (const auto& entry : *it) {
                if (!entry.is_object()) continue;
                SpawnGroupListItem item{};
                item.spawn_id = entry.value("spawn_id", std::string{});
                item.display_name = entry.value("display_name", std::string{});
                if (item.display_name.empty()) {
                    item.display_name = item.spawn_id;
                }
                if (entry.contains("candidates") && entry["candidates"].is_array() && !entry["candidates"].empty()) {
                    const auto& candidate = entry["candidates"][0];
                    if (candidate.is_object()) {
                        std::string asset_name = candidate.value("name", std::string{});
                        if (!asset_name.empty()) {
                            item.icon_label = asset_name.substr(0, std::min<std::size_t>(2, asset_name.size()));
                        }
                    }
                }
                rows.push_back(std::move(item));
            }
        }
    }

    const std::size_t next_hash = hash_spawn_group_rows(rows);
    if (next_hash == spawn_group_rows_hash_ && rows.size() == spawn_group_rows_.size()) {
        return;
    }
    spawn_group_rows_hash_ = next_hash;
    spawn_group_rows_ = rows;
    if (auto* widget = dynamic_cast<SpawnGroupListWidget*>(spawn_group_list_widget_.get())) {
        widget->set_rows(spawn_group_rows_);
    }
    refresh_base_panel_rows();
    request_container_layout();
}

void RoomConfigurator::request_container_layout() {
    if (container_) {
        container_->request_layout();
    }
}

void RoomConfigurator::prune_collapsible_caches() {
    std::unordered_set<const DockableCollapsible*> active;
    active.reserve(ordered_base_panels_.size());
    for (auto* panel : ordered_base_panels_) {
        if (panel) active.insert(panel);
    }

    for (auto it = collapsible_height_cache_.begin(); it != collapsible_height_cache_.end();) {
        if (active.find(it->first) == active.end()) {
            it = collapsible_height_cache_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = base_panel_keys_.begin(); it != base_panel_keys_.end();) {
        if (active.find(it->first) == active.end()) {
            it = base_panel_keys_.erase(it);
        } else {
            ++it;
        }
    }
}

int RoomConfigurator::cached_collapsible_height(const DockableCollapsible* panel) const {
    if (!panel) return 0;
    auto it = collapsible_height_cache_.find(panel);
    if (it != collapsible_height_cache_.end() && it->second > 0) {
        return it->second;
    }
    int h = panel->height();
    if (h <= 0) {
        h = DMButton::height() + 2 * DMSpacing::panel_padding();
    }
    return h;
}

void RoomConfigurator::update_collapsible_height_cache(const DockableCollapsible* panel, int new_height) {
    if (!panel) return;
    int clamped = std::max(new_height, DMButton::height());
    auto it = collapsible_height_cache_.find(panel);
    if (it != collapsible_height_cache_.end() && it->second == clamped) {
        return;
    }
    collapsible_height_cache_[panel] = clamped;
    request_container_layout();
}

void RoomConfigurator::forget_collapsible(const DockableCollapsible* panel) {
    if (!panel) return;
    collapsible_height_cache_.erase(panel);
    base_panel_keys_.erase(panel);
}

bool RoomConfigurator::base_panel_expanded(const std::string& key) const {
    auto it = base_panel_expanded_state_.find(key);
    if (it != base_panel_expanded_state_.end()) {
        return it->second;
    }
    return false;
}

void RoomConfigurator::set_base_panel_expanded(const std::string& key, bool expanded) {
    base_panel_expanded_state_[key] = expanded;
}

void RoomConfigurator::apply_panel_focus_states() {
    auto panel_is_active = [this](DockableCollapsible* candidate) -> bool {
        if (!candidate) {
            return false;
        }
        for (auto* panel : ordered_base_panels_) {
            if (panel == candidate) {
                return true;
            }
        }
        return false;
};

    if (focused_panel_ && !panel_is_active(focused_panel_)) {
        focused_panel_ = nullptr;
    }

    auto apply_state = [&](DockableCollapsible* panel) {
        if (!panel) {
            return;
        }
        const bool focused = (panel == focused_panel_);
        panel->set_embedded_focus_state(focused);
        panel->set_embedded_interaction_enabled(panel->is_visible());
};

    for (auto* panel : ordered_base_panels_) {
        apply_state(panel);
    }
}

void RoomConfigurator::focus_panel(DockableCollapsible* panel, bool expand_on_focus) {
    auto is_active = [this](DockableCollapsible* candidate) -> bool {
        if (!candidate) {
            return false;
        }
        for (auto* base : ordered_base_panels_) {
            if (base == candidate) {
                return true;
            }
        }
        return false;
};

    DockableCollapsible* resolved = (panel && is_active(panel)) ? panel : nullptr;
    DockableCollapsible* previous = focused_panel_;
    focused_panel_ = resolved;
    apply_panel_focus_states();
    if (focused_panel_) {
        focused_panel_->force_pointer_ready();
        if (expand_on_focus && !focused_panel_->is_expanded()) {
            focused_panel_->set_expanded(true);
        }
    }
    if (previous != focused_panel_) {
        request_container_layout();
    }
}

void RoomConfigurator::clear_panel_focus() {
    focus_panel(nullptr);
}

DockableCollapsible* RoomConfigurator::panel_at_point(SDL_Point p) const {
    for (size_t i = 0; i < ordered_base_panels_.size(); ++i) {
        auto* panel = ordered_base_panels_[i];
        if (!panel || !panel->is_visible()) {
            continue;
        }
        SDL_Rect bounds = (i < ordered_panel_bounds_.size()) ? ordered_panel_bounds_[i] : panel->rect();
        if (bounds.w <= 0 || bounds.h <= 0) {
            bounds = panel->rect();
        }
        if (bounds.w <= 0 || bounds.h <= 0) {
            continue;
        }
        if (SDL_PointInRect(&p, &bounds)) {
            return panel;
        }
    }
    return nullptr;
}

bool RoomConfigurator::handle_panel_focus_event(const SDL_Event& e) {
    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    SDL_Point pointer = sdl_mouse_util::ButtonPoint(e.button);
    DockableCollapsible* target = panel_at_point(pointer);
    if (!target) {
        return false;
    }
    if (target != focused_panel_) {
        focus_panel(target, false);
    }
    // Do not consume the focus click; forward it to the panel so controls respond
    // on first click instead of requiring a second click.
    return false;
}

int RoomConfigurator::layout_content(const SlidingWindowContainer::LayoutContext& ctx) const {
    int y = ctx.content_top;
    ordered_panel_bounds_.resize(ordered_base_panels_.size());
    const int embed_screen_h = last_screen_h_ > 0 ? last_screen_h_ : std::max(1, ctx.content_width);
    size_t panel_index = 0;
    for (auto* panel : ordered_base_panels_) {
        if (!panel || !panel->is_visible()) {
            if (panel_index < ordered_panel_bounds_.size()) {
                ordered_panel_bounds_[panel_index] = SDL_Rect{0,0,0,0};
            }
            ++panel_index;
            continue;
        }
        int panel_height = panel->embedded_height(ctx.content_width, embed_screen_h);
        SDL_Rect rect{ctx.content_x, y - ctx.scroll_value, ctx.content_width, panel_height};
        panel->set_rect(rect);
        if (panel_index < ordered_panel_bounds_.size()) {
            ordered_panel_bounds_[panel_index] = rect;
        }
        y += panel_height + ctx.gap;
        ++panel_index;
    }

    return y + ctx.gap;
}

void RoomConfigurator::handle_container_closed() {
    for (auto* panel : ordered_base_panels_) {
        if (panel) {
            panel->set_visible(false);
        }
    }
    clear_panel_focus();
    external_room_json_ = nullptr;
    on_external_change_ = {};
    if (on_close_) on_close_();
}

bool RoomConfigurator::apply_room_data(const nlohmann::json& data) {
    const nlohmann::json& source = data.is_object() ? data : empty_object();
    const bool allow_height = !is_trail_context_;
    const bool include_trail_connection_sector = !is_trail_context_;
    const bool include_boss = !is_trail_context_;
    const bool include_tags = !room_metadata_only_mode_;

    nlohmann::json source_object = source.is_object() ? source : nlohmann::json::object();
    nlohmann::json source_metadata_raw =
        collect_owned_metadata_fields_raw(source_object, include_tags, allow_height, include_trail_connection_sector);

    State new_state = state_ ? *state_ : State{};
    new_state.load_from_json(source_object, {}, allow_height);

    bool state_changed = false;
    if (!state_) {
        state_changed = true;
    } else {
        state_changed =
            new_state.name != state_->name ||
            new_state.size != state_->size ||
            std::abs(new_state.trail_connection_direction_deg - state_->trail_connection_direction_deg) > 1e-6 ||
            new_state.trail_connection_width_percent != state_->trail_connection_width_percent ||
            new_state.is_boss != state_->is_boss ||
            new_state.inherits_assets != state_->inherits_assets ||
            new_state.inherit_map_floor_color != state_->inherit_map_floor_color ||
            !colors_equal(new_state.room_floor_color, state_->room_floor_color) ||
            vibble::weighted_range::to_json(new_state.coarseness) != vibble::weighted_range::to_json(state_->coarseness);
    }

    bool tags_changed = false;
    if (include_tags) {
        std::vector<std::string> prev_include = sorted_strings(room_tags_);
        std::vector<std::string> prev_exclude = is_trail_context_ ? sorted_strings(room_anti_tags_) : std::vector<std::string>{};
        load_tags_from_json(source_object);
        std::vector<std::string> include = sorted_strings(room_tags_);
        std::vector<std::string> exclude = is_trail_context_ ? sorted_strings(room_anti_tags_) : std::vector<std::string>{};
        tags_changed = (include != prev_include) || (exclude != prev_exclude);
    }

    if (!loaded_json_.is_object()) {
        loaded_json_ = nlohmann::json::object();
    }
    if (loaded_json_ != source_object) {
        loaded_json_ = source_object;
    }

    if (!state_) {
        state_ = std::make_unique<State>();
    }
    *state_ = new_state;
    tags_dirty_ = false;
    trail_connection_sector_dirty_ = false;

    state_->apply_to_json(loaded_json_, allow_height, false, include_trail_connection_sector, include_boss);
    if (include_tags) {
        write_tags_to_json(loaded_json_);
    }

    nlohmann::json patched_metadata_raw =
        collect_owned_metadata_fields_raw(loaded_json_, include_tags, allow_height, include_trail_connection_sector);
    const bool needs_persist = (patched_metadata_raw != source_metadata_raw);

    nlohmann::json canonical_metadata = nlohmann::json::object();
    state_->apply_to_json(canonical_metadata, allow_height, false, include_trail_connection_sector, include_boss);
    nlohmann::json new_snapshot = build_metadata_snapshot_json(
        canonical_metadata,
        room_tags_,
        is_trail_context_ ? room_anti_tags_ : std::vector<std::string>{},
        include_tags);
    std::size_t new_snapshot_hash = hash_metadata_snapshot(new_snapshot);
    const bool snapshot_changed = (new_snapshot_hash != metadata_snapshot_hash_);

    if (needs_persist) {
        if (room_ || external_room_json_) {
            nlohmann::json& target = live_room_json();
            if (!target.is_object()) {
                target = nlohmann::json::object();
            }
            state_->apply_to_json(target, allow_height, false, include_trail_connection_sector, include_boss);
            if (include_tags) {
                write_tags_to_json(target);
            }
        }
        if (room_) {
            if (room_save_callback_) {
                room_save_callback_(false);
            } else {
                room_->mark_dirty();
            }
        } else if (external_room_json_ && on_external_change_) {
            on_external_change_();
        }
    }

    metadata_snapshot_ = std::move(new_snapshot);
    metadata_snapshot_hash_ = new_snapshot_hash;

    return state_changed || tags_changed || snapshot_changed || needs_persist;
}

void RoomConfigurator::open(const nlohmann::json& room_data) {
    const bool was_visible = container_ && container_->is_visible();

    if (!was_visible) {
        reset_expanded_state_pending_ = true;
    }

    room_ = nullptr;
    external_room_json_ = nullptr;
    on_external_change_ = {};
    is_trail_context_ = false;
    bool changed = apply_room_data(room_data);
    if (changed || !was_visible) {
        rebuild_rows();
        if (!was_visible) {
            reset_scroll();
        }
    }
    if (container_) {
        container_->open();
    }
}

void RoomConfigurator::open(nlohmann::json& room_data, std::function<void()> on_change) {
    open(room_data, false, std::move(on_change));
}

void RoomConfigurator::open(nlohmann::json& room_data, bool is_trail_context, std::function<void()> on_change) {
    const bool was_visible = container_ && container_->is_visible();

    if (!was_visible) {
        reset_expanded_state_pending_ = true;
    }

    const bool context_changed = is_trail_context_ != is_trail_context;
    room_ = nullptr;
    external_room_json_ = &room_data;
    on_external_change_ = std::move(on_change);
    is_trail_context_ = is_trail_context;
    const bool data_changed = apply_room_data(room_data);
    const bool changed = context_changed || data_changed;
    if (changed || !was_visible) {
        rebuild_rows();
        if (!was_visible) {
            reset_scroll();
        }
    }
    if (container_) {
        container_->open();
    }
}

void RoomConfigurator::open(Room* room) {
    const bool was_visible = container_ && container_->is_visible();

    if (!was_visible) {
        reset_expanded_state_pending_ = true;
    }

    Room* previous = room_;
    room_ = room;
    external_room_json_ = nullptr;
    on_external_change_ = {};
    is_trail_context_ = false;
    if (room_) {
        const std::string& dir = room_->room_directory;
        if (dir.find("trails_data") != std::string::npos) {
            is_trail_context_ = true;
        }
    }

    nlohmann::json source = room ? room->assets_data() : empty_object();
    if (room && source.is_object() && !room->room_name.empty()) {
        source["name"] = room->room_name;
        source.erase("room_name");
    }
    const bool data_changed = apply_room_data(source);
    const bool changed = (room != previous) || data_changed;
    if (changed || !was_visible) {
        rebuild_rows();
        if (!was_visible) {
            reset_scroll();
        }
    }
    if (container_) {
        container_->open();
    }
}

void RoomConfigurator::close() {
    clear_panel_focus();
    if (!container_ || !container_->is_visible()) {
        external_room_json_ = nullptr;
        on_external_change_ = {};
        return;
    }
    container_->close();
}

bool RoomConfigurator::visible() const { return container_ && container_->is_visible(); }

bool RoomConfigurator::any_panel_visible() const { return visible(); }

bool RoomConfigurator::is_locked() const {
    for (auto* panel : ordered_base_panels_) {
        if (panel && panel->isLocked()) {
            return true;
        }
    }
    return false;
}

int RoomConfigurator::selected_size() const {
    if (!state_) return is_trail_context_ ? kDefaultTrailSize : kDefaultRoomSize;
    if (is_trail_context_) {
        if (state_->size < kMinTrailSize || state_->size > kMaxTrailSize) {
            return kDefaultTrailSize;
        }
        return std::clamp(state_->size, kMinTrailSize, kMaxTrailSize);
    }
    return std::clamp(state_->size, kMinRoomSize, kMaxRoomSize);
}

void RoomConfigurator::rebuild_rows() {
    if (!state_) {
        state_ = std::make_unique<State>();
    }

    int previous_scroll = container_ ? container_->scroll_value() : 0;

    if (rebuild_in_progress_) {
        pending_rebuild_ = true;
        return;
    }

    for (;;) {
        rebuild_in_progress_ = true;
        do {
            pending_rebuild_ = false;
            rebuild_rows_internal();
        } while (pending_rebuild_);
        rebuild_in_progress_ = false;
        if (!pending_rebuild_) {
            break;
        }

        static int guard_counter = 0;
        if (++guard_counter > 8) {
            deferred_rebuild_ = true;
            guard_counter = 0;
            break;
        }
    }

    if (container_) {
        configure_container(*container_);
        container_->set_scroll_value(previous_scroll);
    }
}

void RoomConfigurator::rebuild_rows_internal() {
    if (!state_) {
        state_ = std::make_unique<State>();
    }

    bool force_collapse_sections = reset_expanded_state_pending_;
    if (reset_expanded_state_pending_) {
        base_panel_expanded_state_.clear();
        collapsible_height_cache_.clear();
        base_panel_keys_.clear();
    }
    reset_expanded_state_pending_ = false;

    ensure_base_panels();
    ordered_base_panels_.clear();

    name_box_ = std::make_unique<DMTextBox>(is_trail_context_ ? "Trail Name" : "Room Name", state_->name);
    name_widget_ = std::make_unique<TextBoxWidget>(name_box_.get());

    size_stepper_ = std::make_unique<DMNumericStepper>(
        "Size",
        is_trail_context_ ? kMinTrailSize : kMinRoomSize,
        is_trail_context_ ? kMaxTrailSize : kMaxRoomSize,
        selected_size());
    size_stepper_->set_step(1);
    size_widget_ = std::make_unique<StepperWidget>(size_stepper_.get());

    coarseness_range_widget_ = std::make_unique<DMWeightedRangeWidget>(
        "Coarseness Radius", state_->coarseness, kMinCoarseness, kMaxCoarseness, false);
    coarseness_widget_ = std::make_unique<WeightedRangeWidget>(coarseness_range_widget_.get());
    if (!is_trail_context_) {
        trail_connection_sector_widget_ = std::make_unique<TrailConnectionSectorWidget>(
            state_->trail_connection_direction_deg,
            state_->trail_connection_width_percent,
            [this](double direction_deg, int width_percent) {
                if (!state_) {
                    return;
                }
                const double normalized = normalize_angle_degrees(direction_deg);
                const int clamped =
                    std::clamp(width_percent, kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent);
                if (std::abs(state_->trail_connection_direction_deg - normalized) <= 1e-6 &&
                    state_->trail_connection_width_percent == clamped) {
                    return;
                }
                state_->trail_connection_direction_deg = normalized;
                state_->trail_connection_width_percent = clamped;
                trail_connection_sector_dirty_ = true;
                if (sector_direction_stepper_) {
                    sector_direction_stepper_->set_value(static_cast<int>(std::lround(normalized)));
                }
                if (sector_width_stepper_) {
                    sector_width_stepper_->set_value(clamped);
                }
                request_container_layout();
            });

        sector_direction_stepper_ = std::make_unique<DMNumericStepper>(
            "Sector Direction (deg)", 0, 359, static_cast<int>(std::lround(state_->trail_connection_direction_deg)));
        sector_direction_stepper_->set_step(1);
        sector_direction_widget_ = std::make_unique<StepperWidget>(sector_direction_stepper_.get());

        sector_width_stepper_ = std::make_unique<DMNumericStepper>(
            "Sector Width (%)",
            kTrailSectorMinWidthPercent,
            kTrailSectorMaxWidthPercent,
            state_->trail_connection_width_percent);
        sector_width_stepper_->set_step(1);
        sector_width_widget_ = std::make_unique<StepperWidget>(sector_width_stepper_.get());

        sector_reset_button_ = std::make_unique<DMButton>("Reset Full Sector", &DMStyles::ListButton(), 0, DMButton::height());
        sector_reset_widget_ = std::make_unique<ButtonWidget>(sector_reset_button_.get(), [this]() {
            if (!state_) {
                return;
            }
            state_->trail_connection_direction_deg = kTrailSectorDefaultDirectionDeg;
            state_->trail_connection_width_percent = kTrailSectorDefaultWidthPercent;
            trail_connection_sector_dirty_ = true;
            if (sector_direction_stepper_) {
                sector_direction_stepper_->set_value(static_cast<int>(std::lround(state_->trail_connection_direction_deg)));
            }
            if (sector_width_stepper_) {
                sector_width_stepper_->set_value(state_->trail_connection_width_percent);
            }
            if (trail_connection_sector_widget_) {
                auto* sector_widget = dynamic_cast<TrailConnectionSectorWidget*>(trail_connection_sector_widget_.get());
                if (sector_widget) {
                    sector_widget->set_state(state_->trail_connection_direction_deg, state_->trail_connection_width_percent);
                }
            }
            request_container_layout();
        });
    } else {
        trail_connection_sector_widget_.reset();
        sector_direction_stepper_.reset();
        sector_direction_widget_.reset();
        sector_width_stepper_.reset();
        sector_width_widget_.reset();
        sector_reset_button_.reset();
        sector_reset_widget_.reset();
        trail_connection_sector_dirty_ = false;
    }

    if (!is_trail_context_) {
        boss_checkbox_ = std::make_unique<DMCheckbox>("Boss", state_->is_boss);
        boss_widget_ = std::make_unique<CheckboxWidget>(boss_checkbox_.get());
    } else {
        boss_checkbox_.reset();
        boss_widget_.reset();
    }

    if (!is_trail_context_) {
        inherit_checkbox_ = std::make_unique<DMCheckbox>("Inherit Map Assets", state_->inherits_assets);
        inherit_widget_ = std::make_unique<CheckboxWidget>(inherit_checkbox_.get());
        inherit_floor_color_checkbox_ =
            std::make_unique<DMCheckbox>("Inherit Map Color", state_->inherit_map_floor_color);
        inherit_floor_color_widget_ = std::make_unique<CheckboxWidget>(inherit_floor_color_checkbox_.get());
        room_floor_color_button_ = std::make_unique<DMButton>("Pick Room Floor Color", &DMStyles::AccentButton(), 0, DMButton::height());
        room_floor_color_widget_ = std::make_unique<ButtonWidget>(room_floor_color_button_.get(), [this]() {
            if (!state_) {
                return;
            }
            if (!color_picker_) {
                color_picker_ = std::make_unique<DevColorPicker>();
            }
            color_picker_->set_screen_size(last_screen_w_, last_screen_h_);
            color_picker_->open(state_->room_floor_color, [this](SDL_Color chosen) {
                if (!state_) {
                    return;
                }
                chosen.a = 255;
                state_->room_floor_color = chosen;
                room_floor_color_dirty_ = true;
            });
        });
    } else {
        inherit_checkbox_.reset();
        inherit_widget_.reset();
        inherit_floor_color_checkbox_.reset();
        inherit_floor_color_widget_.reset();
        room_floor_color_button_.reset();
        room_floor_color_widget_.reset();
        room_floor_color_dirty_ = false;
    }

    spawn_group_list_widget_ = std::make_unique<SpawnGroupListWidget>(
        [this](const std::string& spawn_id) {
            if (on_spawn_group_click_) {
                on_spawn_group_click_(spawn_id);
            }
        },
        [this](const std::string& spawn_id) {
            if (on_spawn_group_double_click_) {
                on_spawn_group_double_click_(spawn_id);
            }
        },
        [this](const std::string& spawn_id) -> bool {
            if (!on_spawn_group_delete_) {
                return false;
            }
            const bool removed = on_spawn_group_delete_(spawn_id);
            request_rebuild();
            return removed;
        });

    if (!room_metadata_only_mode_) {
        tag_editor_ = std::make_unique<TagEditorWidget>();
        tag_editor_->set_tags(room_tags_, is_trail_context_ ? room_anti_tags_ : std::vector<std::string>{});
        if (!is_trail_context_) {
            tag_editor_->set_recommended_tags(collect_room_tag_recommendations(assets_, state_ ? state_->name : std::string{}));
        }
        tag_editor_->set_on_changed([this](const std::vector<std::string>& include,
                                           const std::vector<std::string>& exclude) {
            const std::vector<std::string> normalized_exclude = is_trail_context_ ? exclude : std::vector<std::string>{};
            if (include != room_tags_ || normalized_exclude != room_anti_tags_) {
                room_tags_ = include;
                room_anti_tags_ = normalized_exclude;
                tags_dirty_ = true;
                this->request_container_layout();
            }
        });
    } else {
        tag_editor_.reset();
        room_tags_.clear();
        room_anti_tags_.clear();
        tags_dirty_ = false;
    }

    refresh_base_panel_rows();
    refresh_spawn_group_rows_if_needed();
    if (!focused_panel_ && geometry_panel_ && geometry_panel_->is_visible()) {
        focus_panel(geometry_panel_.get());
    }

    if (force_collapse_sections) {
        for (auto* panel : ordered_base_panels_) {
            if (panel) {
                panel->force_pointer_ready();
            }
        }
    }

    prune_collapsible_caches();
    request_container_layout();
}

void RoomConfigurator::update(const Input& input, int screen_w, int screen_h) {
    last_screen_w_ = screen_w;
    last_screen_h_ = screen_h;
    if (color_picker_) {
        color_picker_->set_screen_size(screen_w, screen_h);
    }
    if (deferred_rebuild_ && !rebuild_in_progress_) {
        deferred_rebuild_ = false;
        rebuild_rows();
    }
    ensure_base_panels();
    const bool panel_visible = container_ && container_->is_visible();
    SDL_Rect panel_work_area = work_area_;
    if (panel_work_area.w <= 0 || panel_work_area.h <= 0) {
        panel_work_area = SDL_Rect{0, 0, screen_w, screen_h};
    }
    for (auto* panel : ordered_base_panels_) {
        if (!panel) continue;
        panel->set_visible(panel_visible);
        if (panel_work_area.w > 0 && panel_work_area.h > 0) {
            panel->set_work_area(panel_work_area);
        }
    }
    if (container_) {
        container_->update(input, screen_w, screen_h);
    }

    for (auto* panel : ordered_base_panels_) {
        if (!panel) continue;
        update_collapsible_height_cache(panel, panel->height());
        auto it = base_panel_keys_.find(panel);
        if (it != base_panel_keys_.end()) {
            set_base_panel_expanded(it->second, panel->is_expanded());
        }
    }
    if (!state_) return;

    bool needs_rebuild = sync_state_from_widgets();
    if (needs_rebuild) {
        rebuild_rows();
    } else if (deferred_rebuild_) {
        deferred_rebuild_ = false;
        rebuild_rows();
    }
}

void RoomConfigurator::prepare_for_event(int screen_w, int screen_h) {
    int use_w = screen_w > 0 ? screen_w : (last_screen_w_ > 0 ? last_screen_w_ : 0);
    int use_h = screen_h > 0 ? screen_h : (last_screen_h_ > 0 ? last_screen_h_ : 0);
    if (use_w <= 0 || use_h <= 0) {
        return;
    }
    ensure_base_panels();
    last_screen_w_ = use_w;
    last_screen_h_ = use_h;
    if (color_picker_) {
        color_picker_->set_screen_size(use_w, use_h);
    }
    const bool panel_visible = container_ && container_->is_visible();
    SDL_Rect panel_work_area = work_area_;
    if (panel_work_area.w <= 0 || panel_work_area.h <= 0) {
        panel_work_area = SDL_Rect{0, 0, use_w, use_h};
    }
    for (auto* panel : ordered_base_panels_) {
        if (panel) {
            panel->set_visible(panel_visible);
            if (panel_work_area.w > 0 && panel_work_area.h > 0) {
                panel->set_work_area(panel_work_area);
            }
        }
    }
    if (container_) {
        container_->prepare_layout(use_w, use_h);
    }
}

void RoomConfigurator::expand_width_slider_range_if_needed() {}


void RoomConfigurator::expand_height_slider_range_if_needed() {}

bool RoomConfigurator::sync_state_from_widgets() {
    if (!state_) return false;

    bool changed = false;
    bool rebuild_required = false;
    bool tags_changed = false;
    const bool allow_height = !is_trail_context_;
    const bool include_trail_connection_sector = !is_trail_context_;
    const bool include_boss = !is_trail_context_;

    if (!room_metadata_only_mode_ && tags_dirty_) {
        changed = true;
        tags_dirty_ = false;
        tags_changed = true;
    } else if (room_metadata_only_mode_) {
        tags_dirty_ = false;
    }

    if (trail_connection_sector_dirty_) {
        changed = true;
        trail_connection_sector_dirty_ = false;
    }
    if (room_floor_color_dirty_) {
        changed = true;
        room_floor_color_dirty_ = false;
    }
    if (name_box_ && !name_box_->is_editing()) {
        std::string new_name = name_box_->value();
        if (new_name != state_->name) {
            std::string final_name = new_name;
            if (on_room_renamed_) {
                try {
                    final_name = on_room_renamed_(state_->name, new_name);
                } catch (...) {
                    final_name = new_name;
                }
            }
            if (final_name != new_name && name_box_) {
                name_box_->set_value(final_name);
            }
            state_->name = std::move(final_name);
            changed = true;
        }
    }

    if (size_stepper_) {
        int value = size_stepper_->value();
        if (is_trail_context_) {
            value = (value < kMinTrailSize || value > kMaxTrailSize)
                ? kDefaultTrailSize
                : std::clamp(value, kMinTrailSize, kMaxTrailSize);
        } else {
            value = std::clamp(value, kMinRoomSize, kMaxRoomSize);
        }
        if (value != state_->size) {
            state_->size = value;
            changed = true;
        }
    }
    if (coarseness_range_widget_) {
        const auto value = coarseness_range_widget_->value();
        if (vibble::weighted_range::to_json(value) != vibble::weighted_range::to_json(state_->coarseness)) {
            state_->coarseness = value;
            changed = true;
        }
    }
    if (sector_direction_stepper_) {
        int v = std::clamp(sector_direction_stepper_->value(), 0, 359);
        const double normalized = normalize_angle_degrees(static_cast<double>(v));
        if (std::abs(normalized - state_->trail_connection_direction_deg) > 1e-6) {
            state_->trail_connection_direction_deg = normalized;
            changed = true;
        }
    }

    if (sector_width_stepper_) {
        int v = std::clamp(sector_width_stepper_->value(), kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent);
        if (v != state_->trail_connection_width_percent) {
            state_->trail_connection_width_percent = v;
            changed = true;
        }
    }

    if (boss_checkbox_) {
        bool value = boss_checkbox_->value();
        if (value != state_->is_boss) {
            state_->is_boss = value;
            changed = true;
        }
    }

    if (inherit_checkbox_) {
        bool value = inherit_checkbox_->value();
        if (value != state_->inherits_assets) {
            state_->inherits_assets = value;
            changed = true;
        }
    }
    if (inherit_floor_color_checkbox_) {
        bool value = inherit_floor_color_checkbox_->value();
        if (value != state_->inherit_map_floor_color) {
            if (!value && state_->inherit_map_floor_color) {
                SDL_Color map_default{0, 0, 0, 255};
                if (assets_) {
                    const nlohmann::json& map_info = assets_->map_info_json();
                    if (map_info.is_object() && map_info.contains("dev_map_settings")) {
                        const auto& dev = map_info["dev_map_settings"];
                        if (dev.is_object() && dev.contains("default_floor_color")) {
                            if (auto parsed = utils::color::color_from_json(dev["default_floor_color"])) {
                                map_default = *parsed;
                                map_default.a = 255;
                            }
                        }
                    }
                }
                state_->room_floor_color = map_default;
            }
            state_->inherit_map_floor_color = value;
            changed = true;
            rebuild_required = true;
        }
    }

    if (state_->ensure_valid(allow_height, true)) {
        changed = true;
    }

    if (trail_connection_sector_widget_) {
        auto* sector_widget = dynamic_cast<TrailConnectionSectorWidget*>(trail_connection_sector_widget_.get());
        if (sector_widget) {
            sector_widget->set_state(state_->trail_connection_direction_deg, state_->trail_connection_width_percent);
        }
    }
    if (sector_direction_stepper_) {
        sector_direction_stepper_->set_value(static_cast<int>(std::lround(state_->trail_connection_direction_deg)));
    }
    if (sector_width_stepper_) {
        sector_width_stepper_->set_value(state_->trail_connection_width_percent);
    }

    if (size_stepper_) {
        size_stepper_->set_value(state_->size);
    }
    if (coarseness_range_widget_) {
        coarseness_range_widget_->set_value(state_->coarseness);
    }
    if (changed) {
        const bool include_tags = !room_metadata_only_mode_;
        nlohmann::json canonical_metadata = nlohmann::json::object();
        state_->apply_to_json(canonical_metadata, allow_height, false, include_trail_connection_sector, include_boss);
        nlohmann::json new_snapshot = build_metadata_snapshot_json(
            canonical_metadata,
            room_tags_,
            is_trail_context_ ? room_anti_tags_ : std::vector<std::string>{},
            include_tags);
        std::size_t new_snapshot_hash = hash_metadata_snapshot(new_snapshot);
        if (new_snapshot_hash != metadata_snapshot_hash_) {
            if (!loaded_json_.is_object()) {
                loaded_json_ = nlohmann::json::object();
            }
            state_->apply_to_json(loaded_json_, allow_height, false, include_trail_connection_sector, include_boss);
            if (include_tags) {
                write_tags_to_json(loaded_json_);
            }

            if (room_ || external_room_json_) {
                auto& root = live_room_json();
                state_->apply_to_json(root, allow_height, false, include_trail_connection_sector, include_boss);
                if (include_tags) {
                    write_tags_to_json(root);
                }
            }

            if (room_) {
                if (room_save_callback_) { room_save_callback_(false); } else { room_->mark_dirty(); }
                if (tags_changed) {
                    tag_utils::notify_tags_changed();
                }
            } else if (external_room_json_ && on_external_change_) {
                on_external_change_();
            }

            metadata_snapshot_ = std::move(new_snapshot);
            metadata_snapshot_hash_ = new_snapshot_hash;
        }
    }

    return rebuild_required;
}

bool RoomConfigurator::handle_event(const SDL_Event& e) {
    if (!container_ || !container_->is_visible()) return false;
    if (DMWeightedRangeWidget::has_active_expanded()) {
        if (DMWeightedRangeWidget::handle_active_expanded_event(e)) {
            return true;
        }
        switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_TEXT_INPUT:
            return true;
        default:
            break;
        }
    }
    if (color_picker_ && color_picker_->is_open()) {
        return color_picker_->handle_event(e);
    }
    if (last_screen_w_ > 0 && last_screen_h_ > 0) {
        prepare_for_event(last_screen_w_, last_screen_h_);
    }
    return container_->handle_event(e);
}

void RoomConfigurator::render(SDL_Renderer* r) const {
    if (!container_ || !container_->is_visible()) return;
    container_->render(r, last_screen_w_, last_screen_h_);
    DMDropdown::render_active_options(r);
    if (color_picker_ && color_picker_->is_open()) {
        color_picker_->render(r);
    }
    DMWeightedRangeWidget::render_active_expanded(r);
}

const SDL_Rect& RoomConfigurator::panel_rect() const {
    if (!container_) {
        static SDL_Rect empty{0, 0, 0, 0};
        return empty;
    }
    return container_->panel_rect();
}

std::string RoomConfigurator::current_header_text() const {
    if (state_ && !state_->name.empty()) {
        if (is_trail_context_) {
            return std::string{"Trail: "} + state_->name;
        }
        return std::string{"Room: "} + state_->name;
    }
    return is_trail_context_ ? std::string{"Trail Config"} : std::string{"Room Config"};
}

const nlohmann::json& RoomConfigurator::live_room_json() const {
    if (room_) {
        return room_->assets_data();
    }
    if (external_room_json_) {
        return *external_room_json_;
    }
    return loaded_json_;
}

nlohmann::json& RoomConfigurator::live_room_json() {
    if (room_) {
        return room_->assets_data();
    }
    if (external_room_json_) {
        return *external_room_json_;
    }
    if (!loaded_json_.is_object()) {
        loaded_json_ = nlohmann::json::object();
    }
    return loaded_json_;
}

nlohmann::json RoomConfigurator::build_json() const {
    nlohmann::json result = loaded_json_.is_object() ? loaded_json_ : nlohmann::json::object();
    if (state_) {
        State copy = *state_;
        const bool allow_height = !is_trail_context_;
        const bool include_trail_connection_sector = !is_trail_context_;
        const bool include_boss = !is_trail_context_;
        copy.ensure_valid(allow_height);
        copy.apply_to_json(result, allow_height, !room_metadata_only_mode_, include_trail_connection_sector, include_boss);
        if (!room_metadata_only_mode_) {
            write_tags_to_json(result);
        }
    }
    return result;
}

bool RoomConfigurator::is_point_inside(int x, int y) const {
    return container_ && container_->is_point_inside(x, y);
}

void RoomConfigurator::load_tags_from_json(const nlohmann::json& data) {
    std::set<std::string> include;
    std::set<std::string> exclude;

    auto read_array = [&](const nlohmann::json& arr, std::set<std::string>& dest) {
        if (!arr.is_array()) return;
        for (const auto& entry : arr) {
            if (!entry.is_string()) continue;
            std::string normalized = tag_utils::normalize(entry.get<std::string>());
            if (!normalized.empty()) dest.insert(std::move(normalized));
        }
};

    if (data.is_object()) {
        if (data.contains("room_tags")) {
            read_array(data["room_tags"], include);
        }
        if (data.contains("tags")) {
            const auto& section = data["tags"];
            if (section.is_object()) {
                if (section.contains("include")) read_array(section["include"], include);
                if (section.contains("tags")) read_array(section["tags"], include);
                if (is_trail_context_) {
                    if (section.contains("exclude")) read_array(section["exclude"], exclude);
                    if (section.contains("anti_tags")) read_array(section["anti_tags"], exclude);
                }
            } else if (section.is_array()) {
                read_array(section, include);
            }
        }
        if (is_trail_context_ && data.contains("anti_tags")) {
            read_array(data["anti_tags"], exclude);
        }
    }

    room_tags_.assign(include.begin(), include.end());
    if (is_trail_context_) {
        room_anti_tags_.assign(exclude.begin(), exclude.end());
    } else {
        room_anti_tags_.clear();
    }
}

void RoomConfigurator::write_tags_to_json(nlohmann::json& object) const {
    if (!object.is_object()) {
        object = nlohmann::json::object();
    }
    if (is_trail_context_) {
        if (room_tags_.empty() && room_anti_tags_.empty()) {
            object.erase("tags");
            object.erase("anti_tags");
            object.erase("room_tags");
            return;
        }

        nlohmann::json section = nlohmann::json::object();
        if (!room_tags_.empty()) {
            section["include"] = room_tags_;
        }
        if (!room_anti_tags_.empty()) {
            section["exclude"] = room_anti_tags_;
        }
        object["tags"] = std::move(section);
        object.erase("anti_tags");
        object["room_tags"] = room_tags_;
        return;
    }

    object["room_tags"] = room_tags_;
    object.erase("tags");
    object.erase("anti_tags");
}

bool RoomConfigurator::focus_name_field() {
    if (!visible() || !name_box_) {
        return false;
    }
    focus_panel(geometry_panel_.get());
    name_box_->start_editing();
    return true;
}

void RoomConfigurator::request_rebuild() {
    deferred_rebuild_ = true;
}

bool RoomConfigurator::camera_controls_enabled() const {
    return false;
}

void RoomConfigurator::refresh_camera_panel_widgets() {
    // Camera section is intentionally removed from Room Config UI.
}

bool RoomConfigurator::apply_camera_adjustment(const CameraAdjustment& adjustment) {
    (void)adjustment;
    return false;
}

void RoomConfigurator::reload_camera_state_from_room() {
    // Camera data is intentionally removed from Room Config state.
}

void RoomConfigurator::request_camera_live_update() {
    // Camera data is intentionally removed from Room Config state.
}
