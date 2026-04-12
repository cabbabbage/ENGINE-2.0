#include "room_configurator.hpp"

#include "DockableCollapsible.hpp"
#include "dm_styles.hpp"
#include "gameplay/map_generation/room.hpp"
#include "tag_editor_widget.hpp"
#include "tag_utils.hpp"
#include "utils/input.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "widgets.hpp"
#include "font_cache.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>
#include <utility>

namespace {
constexpr int kRoomConfigPanelMinWidth = 260;
constexpr bool kTrailsAllowIndependentDimensions = true;
constexpr int kMinRoomDimension = 1;
constexpr int kMaxRoomDimension = 4000;
constexpr int kSliderExpansionMargin = 64;
constexpr int kSliderExpansionFactor = 2;

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

std::optional<int> read_legacy_radius_value(const nlohmann::json& obj) {
    if (!obj.is_object()) {
        return std::nullopt;
    }
    if (auto value = read_json_int(obj, "radius")) {
        return std::max(0, *value);
    }
    return std::nullopt;
}

bool append_unique(std::vector<std::string>& options, const std::string& value) {
    if (value.empty()) return false;
    if (std::find(options.begin(), options.end(), value) != options.end()) {
        return false;
    }
    options.push_back(value);
    return true;
}

}

struct RoomConfigurator::State {
    std::string name;
    std::string geometry;
    int width_min = 500;
    int width_max = kMaxRoomDimension;
    int height_min = 500;
    int height_max = kMaxRoomDimension;
    int edge_smoothness = 2;
    int curvyness = 2;
    bool is_spawn = false;
    bool is_boss = false;
    bool inherits_assets = false;

    bool geometry_is_circle() const { return lowercase_copy(geometry) == "circle"; }

    bool ensure_valid(bool allow_height, bool enforce_dimensions = true) {
        bool mutated = false;
        if (enforce_dimensions) {
            if (width_min > width_max) {
                std::swap(width_min, width_max);
                mutated = true;
            }
            if (height_min > height_max) {
                std::swap(height_min, height_max);
                mutated = true;
            }
        }

        const int clamped_width_min = std::clamp(width_min, kMinRoomDimension, kMaxRoomDimension);
        const int clamped_width_max = std::clamp(width_max, kMinRoomDimension, kMaxRoomDimension);
        const int clamped_height_min = std::clamp(height_min, kMinRoomDimension, kMaxRoomDimension);
        const int clamped_height_max = std::clamp(height_max, kMinRoomDimension, kMaxRoomDimension);

        if (clamped_width_min != width_min) {
            width_min = clamped_width_min;
            mutated = true;
        }
        if (clamped_width_max != width_max) {
            width_max = clamped_width_max;
            mutated = true;
        }
        if (clamped_height_min != height_min) {
            height_min = clamped_height_min;
            mutated = true;
        }
        if (clamped_height_max != height_max) {
            height_max = clamped_height_max;
            mutated = true;
        }

        if (width_min > width_max) {
            width_max = width_min;
            mutated = true;
        }
        if (height_min > height_max) {
            height_max = height_min;
            mutated = true;
        }

        if (!allow_height) {
            if (height_min != width_min) {
                height_min = width_min;
                mutated = true;
            }
            if (height_max != width_max) {
                height_max = width_max;
                mutated = true;
            }
        }

        int new_edge = std::clamp(edge_smoothness, 0, 101);
        if (new_edge != edge_smoothness) {
            edge_smoothness = new_edge;
            mutated = true;
        }
        int new_curvy = std::max(0, curvyness);
        if (new_curvy != curvyness) {
            curvyness = new_curvy;
            mutated = true;
        }
        if (is_spawn && is_boss) {
            is_boss = false;
            mutated = true;
        }
        return mutated;
    }

    void load_from_json(const nlohmann::json& data,
                        const std::vector<std::string>& geometry_options,
                        bool allow_height) {
        const nlohmann::json& src = data.is_object() ? data : empty_object();
        if (auto value = read_json_string(src, "name")) {
            name = *value;
        } else if (auto value = read_json_string(src, "room_name")) {
            name = *value;
        } else {
            name.clear();
        }

        if (auto value = read_json_string(src, "geometry")) {
            geometry = *value;
        } else {
            geometry = geometry_options.empty() ? std::string{} : geometry_options.front();
        }
        if (geometry.empty()) {
            geometry = geometry_options.empty() ? std::string{"Square"} : geometry_options.front();
        }

        bool has_min_width = false;
        bool has_max_width = false;
        bool has_min_height = false;
        bool has_max_height = false;

        if (auto value = read_json_int(src, "min_width")) {
            width_min = *value;
            has_min_width = true;
        }
        if (auto value = read_json_int(src, "max_width")) {
            width_max = *value;
            has_max_width = true;
        }
        if (allow_height) {
            if (auto value = read_json_int(src, "min_height")) {
                height_min = *value;
                has_min_height = true;
            }
            if (auto value = read_json_int(src, "max_height")) {
                height_max = *value;
                has_max_height = true;
            }
        }

        int legacy_min_radius = 0;
        int legacy_max_radius = 0;
        bool has_legacy_min_radius = false;
        bool has_legacy_max_radius = false;
        if (auto value = read_json_int(src, "min_radius")) {
            legacy_min_radius = std::max(0, *value);
            has_legacy_min_radius = true;
        }
        if (auto value = read_json_int(src, "max_radius")) {
            legacy_max_radius = std::max(0, *value);
            has_legacy_max_radius = true;
        }
        if (auto value = read_legacy_radius_value(src)) {
            if (!has_legacy_min_radius) {
                legacy_min_radius = *value;
                has_legacy_min_radius = true;
            }
            if (!has_legacy_max_radius) {
                legacy_max_radius = *value;
                has_legacy_max_radius = true;
            }
        }

        if (geometry_is_circle() && (has_legacy_min_radius || has_legacy_max_radius)) {
            if (legacy_min_radius <= 0 && legacy_max_radius > 0) {
                legacy_min_radius = legacy_max_radius;
            }
            if (legacy_max_radius <= 0 && legacy_min_radius > 0) {
                legacy_max_radius = legacy_min_radius;
            }
            if (legacy_max_radius < legacy_min_radius) {
                std::swap(legacy_min_radius, legacy_max_radius);
            }

            const int legacy_min_diameter = legacy_min_radius > 0 ? legacy_min_radius * 2 : 0;
            const int legacy_max_diameter = legacy_max_radius > 0 ? legacy_max_radius * 2 : legacy_min_diameter;

            if (!has_min_width && legacy_min_diameter > 0) {
                width_min = legacy_min_diameter;
            }
            if (!has_max_width && legacy_max_diameter > 0) {
                width_max = legacy_max_diameter;
            }
            if (allow_height) {
                if (!has_min_height && legacy_min_diameter > 0) {
                    height_min = legacy_min_diameter;
                }
                if (!has_max_height && legacy_max_diameter > 0) {
                    height_max = legacy_max_diameter;
                }
            }
        }

        if (auto value = read_json_bool(src, "is_spawn")) {
            is_spawn = *value;
        } else {
            is_spawn = false;
        }
        if (auto value = read_json_bool(src, "is_boss")) {
            is_boss = *value;
        } else {
            is_boss = false;
        }
        if (auto value = read_json_bool(src, "inherits_map_assets")) {
            inherits_assets = *value;
        } else {
            inherits_assets = false;
        }
        if (auto value = read_json_int(src, "edge_smoothness")) {
            edge_smoothness = *value;
        } else {
            edge_smoothness = 4;
        }
        if (src.contains("curvyness")) {
            if (auto cv = read_json_int(src, "curvyness")) {
                curvyness = std::max(0, *cv);
            }
        } else {
            curvyness = edge_smoothness;  // Only set default when no value exists
        }

        ensure_valid(allow_height);
    }

    void apply_to_json(nlohmann::json& dest, bool allow_height, bool include_camera = true) const {
        if (!dest.is_object()) dest = nlohmann::json::object();
        dest["name"] = name;
        dest["geometry"] = geometry;
        dest["is_spawn"] = is_spawn;
        dest["is_boss"] = is_boss;
        dest["inherits_map_assets"] = inherits_assets;
        dest["edge_smoothness"] = edge_smoothness;
        if (allow_height) {
            dest["curvyness"] = curvyness;
        } else {
            dest.erase("curvyness");
        }

        // Camera fields are authored by RoomEditor shortcuts (Ctrl+A/Ctrl+R).
        // RoomConfigurator no longer owns camera state, so preserve any existing keys.
        (void)include_camera;

        dest.erase("radius");
        dest.erase("min_radius");
        dest.erase("max_radius");
        dest["min_width"] = width_min;
        dest["max_width"] = width_max;
        dest["min_height"] = allow_height ? height_min : width_min;
        dest["max_height"] = allow_height ? height_max : width_max;
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
        snapshot["tags"] = sorted_strings_json(tags);
        snapshot["anti_tags"] = sorted_strings_json(anti_tags);
    }
    return snapshot;
}

std::size_t hash_metadata_snapshot(const nlohmann::json& snapshot) {
    return std::hash<std::string>{}(snapshot.dump());
}

nlohmann::json collect_owned_metadata_fields_raw(const nlohmann::json& source,
                                                 bool include_tags,
                                                 bool allow_height) {
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
    copy_field("geometry");
    copy_field("min_width");
    copy_field("max_width");
    copy_field("min_height");
    copy_field("max_height");
    copy_field("radius");
    copy_field("min_radius");
    copy_field("max_radius");
    copy_field("edge_smoothness");
    if (allow_height) {
        copy_field("curvyness");
    }
    copy_field("is_spawn");
    copy_field("is_boss");
    copy_field("inherits_map_assets");
    if (include_tags) {
        copy_field("tags");
        copy_field("anti_tags");
    }
    return raw;
}

RoomConfigurator::RoomConfigurator() {
    geometry_options_ = {"Square", "Circle"};
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
    request_container_layout();
}

void RoomConfigurator::set_work_area(const SDL_Rect& bounds) {
    work_area_ = bounds;
    ensure_base_panels();
    if (geometry_panel_) geometry_panel_->set_work_area(bounds);
    if (tags_panel_) tags_panel_->set_work_area(bounds);
    if (types_panel_) types_panel_->set_work_area(bounds);
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
    container.set_header_text_provider([this]() { return this->current_header_text(); });
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
        if (focused_panel_ && focused_panel_->is_visible()) {
            DockableCollapsible* focused_before_event = focused_panel_;
            if (focused_before_event->handle_event(e)) {
                request_container_layout();
                const auto panel_still_active = [this](DockableCollapsible* candidate) {
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
                if (panel_still_active(focused_before_event)) {
                    auto it = base_panel_keys_.find(focused_before_event);
                    if (it != base_panel_keys_.end()) {
                        set_base_panel_expanded(it->second, focused_before_event->is_expanded());
                    }
                }
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
    container.set_blocks_editor_interactions(blocks_editor_interactions_);
    container.set_scrollbar_visible(true);
    container.set_content_clip_enabled(false);
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
            panel->set_embedded_interaction_enabled(false);
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

    const std::string geometry_title = is_trail_context_ ? "Trail Geometry" : "Room Geometry";
    const std::string tags_title = is_trail_context_ ? "Trail Tags" : "Room Tags";
    const std::string types_title = is_trail_context_ ? "Trail Types" : "Room Types";

    ensure_panel(geometry_panel_, "geometry", geometry_title);
    if (!room_metadata_only_mode_) {
        ensure_panel(tags_panel_, "tags", tags_title);
    } else if (tags_panel_) {
        tags_panel_->set_visible(false);
    }
    ensure_panel(types_panel_, "types", types_title);
}

void RoomConfigurator::refresh_base_panel_rows() {
    ordered_base_panels_.clear();
    ordered_panel_bounds_.clear();

    if (geometry_panel_) {
        DockableCollapsible::Rows rows;
        if (name_widget_) rows.push_back({name_widget_.get()});
        if (geometry_widget_) rows.push_back({geometry_widget_.get()});
        if (width_range_widget_) {
            rows.push_back({width_range_widget_.get()});
        }
        if (height_range_widget_) {
            rows.push_back({height_range_widget_.get()});
        }
        if (edge_widget_) rows.push_back({edge_widget_.get()});
        if (curvy_widget_) rows.push_back({curvy_widget_.get()});
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
        if (spawn_widget_) toggles.push_back(spawn_widget_.get());
        if (boss_widget_) toggles.push_back(boss_widget_.get());
        if (inherit_widget_) toggles.push_back(inherit_widget_.get());
        if (!toggles.empty()) {
            rows.push_back(std::move(toggles));
        }
        types_panel_->set_rows(rows);
        types_panel_->set_visible(!rows.empty());
        if (!rows.empty()) {
            ordered_base_panels_.push_back(types_panel_.get());
        }
    }
    apply_panel_focus_states();
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
        panel->set_embedded_interaction_enabled(focused);
};

    for (auto* panel : ordered_base_panels_) {
        apply_state(panel);
    }
}

void RoomConfigurator::focus_panel(DockableCollapsible* panel) {
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
        if (!focused_panel_->is_expanded()) {
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
        focus_panel(target);
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
    const bool allow_height = !is_trail_context_ && kTrailsAllowIndependentDimensions;
    const bool include_tags = !room_metadata_only_mode_;

    nlohmann::json source_object = source.is_object() ? source : nlohmann::json::object();
    nlohmann::json source_metadata_raw = collect_owned_metadata_fields_raw(source_object, include_tags, allow_height);

    State new_state = state_ ? *state_ : State{};
    new_state.load_from_json(source_object, geometry_options_, allow_height);
    bool geometry_added = append_unique(geometry_options_, new_state.geometry);

    bool state_changed = false;
    if (!state_) {
        state_changed = true;
    } else {
        state_changed =
            new_state.name != state_->name ||
            new_state.geometry != state_->geometry ||
            new_state.width_min != state_->width_min ||
            new_state.width_max != state_->width_max ||
            new_state.height_min != state_->height_min ||
            new_state.height_max != state_->height_max ||
            new_state.edge_smoothness != state_->edge_smoothness ||
            new_state.curvyness != state_->curvyness ||
            new_state.is_spawn != state_->is_spawn ||
            new_state.is_boss != state_->is_boss ||
            new_state.inherits_assets != state_->inherits_assets;
    }

    bool tags_changed = false;
    if (include_tags) {
        std::vector<std::string> prev_include = sorted_strings(room_tags_);
        std::vector<std::string> prev_exclude = sorted_strings(room_anti_tags_);
        load_tags_from_json(source_object);
        std::vector<std::string> include = sorted_strings(room_tags_);
        std::vector<std::string> exclude = sorted_strings(room_anti_tags_);
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

    state_->apply_to_json(loaded_json_, allow_height, false);
    if (include_tags) {
        write_tags_to_json(loaded_json_);
    }

    nlohmann::json patched_metadata_raw = collect_owned_metadata_fields_raw(loaded_json_, include_tags, allow_height);
    const bool needs_persist = (patched_metadata_raw != source_metadata_raw);

    nlohmann::json canonical_metadata = nlohmann::json::object();
    state_->apply_to_json(canonical_metadata, allow_height, false);
    nlohmann::json new_snapshot = build_metadata_snapshot_json(canonical_metadata, room_tags_, room_anti_tags_, include_tags);
    std::size_t new_snapshot_hash = hash_metadata_snapshot(new_snapshot);
    const bool snapshot_changed = (new_snapshot_hash != metadata_snapshot_hash_);

    if (needs_persist) {
        if (room_ || external_room_json_) {
            nlohmann::json& target = live_room_json();
            if (!target.is_object()) {
                target = nlohmann::json::object();
            }
            state_->apply_to_json(target, allow_height, false);
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

    return geometry_added || state_changed || tags_changed || snapshot_changed || needs_persist;
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
    const bool was_visible = container_ && container_->is_visible();

    if (!was_visible) {
        reset_expanded_state_pending_ = true;
    }

    room_ = nullptr;
    external_room_json_ = &room_data;
    on_external_change_ = std::move(on_change);
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

    const nlohmann::json& source = room ? room->assets_data() : empty_object();
    bool changed = (room != previous) || apply_room_data(source);
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

std::string RoomConfigurator::selected_geometry() const {
    if (!state_) return geometry_options_.empty() ? std::string{} : geometry_options_.front();
    if (geometry_options_.empty()) return state_->geometry;
    auto it = std::find(geometry_options_.begin(), geometry_options_.end(), state_->geometry);
    if (it != geometry_options_.end()) return *it;
    return geometry_options_.front();
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

    bool allow_geometry_choice = !is_trail_context_;
    const bool allow_height = !is_trail_context_ && kTrailsAllowIndependentDimensions;
    if (allow_geometry_choice) {
        auto geom_it = std::find(geometry_options_.begin(), geometry_options_.end(), state_->geometry);
        int geom_index = 0;
        if (geom_it != geometry_options_.end()) {
            geom_index = static_cast<int>(std::distance(geometry_options_.begin(), geom_it));
        }
        geometry_dropdown_ = std::make_unique<DMDropdown>("", geometry_options_, geom_index);
        geometry_widget_ = std::make_unique<DropdownWidget>(geometry_dropdown_.get());
    } else {
        geometry_dropdown_.reset();
        geometry_widget_.reset();
    }

    width_slider_max_range_ = kMaxRoomDimension;
    width_range_slider_ = std::make_unique<DMRangeSlider>(kMinRoomDimension, width_slider_max_range_, state_->width_min, state_->width_max);
    width_range_slider_->set_defer_commit_until_unfocus(true);
    width_range_widget_ = std::make_unique<RangeSliderWidget>(width_range_slider_.get());

    if (allow_height) {
        height_slider_max_range_ = kMaxRoomDimension;
        height_range_slider_ = std::make_unique<DMRangeSlider>(kMinRoomDimension, height_slider_max_range_, state_->height_min, state_->height_max);
        height_range_slider_->set_defer_commit_until_unfocus(true);
        height_range_widget_ = std::make_unique<RangeSliderWidget>(height_range_slider_.get());
    } else {
        height_range_slider_.reset();
        height_range_widget_.reset();
        height_slider_max_range_ = 0;
    }

    if (!is_trail_context_) {
        edge_slider_ = std::make_unique<DMSlider>("Edge Smoothness", 0, 101, state_->edge_smoothness);
        edge_widget_ = std::make_unique<SliderWidget>(edge_slider_.get());
    } else {
        edge_slider_.reset();
        edge_widget_.reset();
    }

    if (is_trail_context_) {
        curvy_slider_ = std::make_unique<DMSlider>("Curvyness", 0, 16, state_->curvyness);
        curvy_widget_ = std::make_unique<SliderWidget>(curvy_slider_.get());
    } else {
        curvy_slider_.reset();
        curvy_widget_.reset();
    }

    if (!is_trail_context_) {
        spawn_checkbox_ = std::make_unique<DMCheckbox>("Spawn", state_->is_spawn);
        spawn_widget_ = std::make_unique<CheckboxWidget>(spawn_checkbox_.get());
    } else {
        spawn_checkbox_.reset();
        spawn_widget_.reset();
    }

    if (!is_trail_context_) {
        boss_checkbox_ = std::make_unique<DMCheckbox>("Boss", state_->is_boss);
        boss_widget_ = std::make_unique<CheckboxWidget>(boss_checkbox_.get());
    } else {
        boss_checkbox_.reset();
        boss_widget_.reset();
    }

    inherit_checkbox_ = std::make_unique<DMCheckbox>("Inherit Map Assets", state_->inherits_assets);
    inherit_widget_ = std::make_unique<CheckboxWidget>(inherit_checkbox_.get());

    if (!room_metadata_only_mode_) {
        tag_editor_ = std::make_unique<TagEditorWidget>();
        tag_editor_->set_tags(room_tags_, room_anti_tags_);
        tag_editor_->set_on_changed([this](const std::vector<std::string>& include,
                                           const std::vector<std::string>& exclude) {
            if (include != room_tags_ || exclude != room_anti_tags_) {
                room_tags_ = include;
                room_anti_tags_ = exclude;
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
    if (container_) {
        container_->prepare_layout(use_w, use_h);
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
}

void RoomConfigurator::expand_width_slider_range_if_needed() {
    if (!width_range_slider_ || !state_) {
        return;
    }
    if (width_slider_max_range_ >= kMaxRoomDimension) {
        return;
    }
    if (state_->width_max + kSliderExpansionMargin < width_slider_max_range_) {
        return;
    }
    int desired = std::max(width_slider_max_range_ * kSliderExpansionFactor, state_->width_max + kSliderExpansionMargin);
    desired = std::min(desired, kMaxRoomDimension);
    if (desired <= width_slider_max_range_) {
        return;
    }
    width_slider_max_range_ = desired;
    width_range_slider_ = std::make_unique<DMRangeSlider>(kMinRoomDimension, width_slider_max_range_, state_->width_min, state_->width_max);
    width_range_slider_->set_defer_commit_until_unfocus(true);
    width_range_widget_ = std::make_unique<RangeSliderWidget>(width_range_slider_.get());
    refresh_base_panel_rows();
    request_container_layout();
}



void RoomConfigurator::expand_height_slider_range_if_needed() {
    if (!height_range_slider_ || !state_) {
        return;
    }
    if (height_slider_max_range_ >= kMaxRoomDimension) {
        return;
    }
    if (state_->height_max + kSliderExpansionMargin < height_slider_max_range_) {
        return;
    }
    int desired = std::max(height_slider_max_range_ * kSliderExpansionFactor, state_->height_max + kSliderExpansionMargin);
    desired = std::min(desired, kMaxRoomDimension);
    if (desired <= height_slider_max_range_) {
        return;
    }
    height_slider_max_range_ = desired;
    height_range_slider_ = std::make_unique<DMRangeSlider>(kMinRoomDimension, height_slider_max_range_, state_->height_min, state_->height_max);
    height_range_slider_->set_defer_commit_until_unfocus(true);
    height_range_widget_ = std::make_unique<RangeSliderWidget>(height_range_slider_.get());
    refresh_base_panel_rows();
    request_container_layout();
}

bool RoomConfigurator::sync_state_from_widgets() {
    if (!state_) return false;

    bool changed = false;
    bool rebuild_required = false;
    bool tags_changed = false;
    const bool allow_height = !is_trail_context_ && kTrailsAllowIndependentDimensions;

    if (!room_metadata_only_mode_ && tags_dirty_) {
        changed = true;
        tags_dirty_ = false;
        tags_changed = true;
    } else if (room_metadata_only_mode_) {
        tags_dirty_ = false;
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

    if (geometry_dropdown_) {
        if (geometry_options_.empty()) {
            geometry_options_.push_back("Square");
        }
        int idx = std::clamp(geometry_dropdown_->selected(), 0, static_cast<int>(geometry_options_.size()) - 1);
        std::string selected = geometry_options_[idx];
        if (selected != state_->geometry) {
            state_->geometry = selected;
            rebuild_required = true;
            changed = true;
        }
    }

    if (width_range_slider_) {
        int slider_min = width_range_slider_->min_value();
        int slider_max = width_range_slider_->max_value();
        if (slider_min != state_->width_min || slider_max != state_->width_max) {
            state_->width_min = slider_min;
            state_->width_max = slider_max;
            changed = true;
        }
        expand_width_slider_range_if_needed();
    }

    if (height_range_slider_) {
        int slider_min = height_range_slider_->min_value();
        int slider_max = height_range_slider_->max_value();
        if (slider_min != state_->height_min || slider_max != state_->height_max) {
            state_->height_min = slider_min;
            state_->height_max = slider_max;
            changed = true;
        }
        expand_height_slider_range_if_needed();
    }
    if (edge_slider_) {
        int v = std::clamp(edge_slider_->value(), 0, 101);
        if (v != state_->edge_smoothness) {
            state_->edge_smoothness = v;
            changed = true;
        }
    }

    if (curvy_slider_) {
        int v = std::max(0, curvy_slider_->value());
        if (v != state_->curvyness) {
            state_->curvyness = v;
            changed = true;
        }
    }

    if (spawn_checkbox_) {
        bool value = spawn_checkbox_->value();
        if (value != state_->is_spawn) {
            state_->is_spawn = value;
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

    if (state_->ensure_valid(allow_height, true)) {
        changed = true;
    }

    if (state_->is_spawn && state_->is_boss) {
        state_->is_boss = false;
        if (boss_checkbox_) boss_checkbox_->set_value(false);
    }

    if (width_range_slider_) {
        bool skip_slider_sync =
            width_range_slider_->defer_commit_until_unfocus() && width_range_slider_->has_pending_values();
        if (!skip_slider_sync) {
            width_range_slider_->set_min_value(state_->width_min);
            width_range_slider_->set_max_value(state_->width_max);
        }
    }
    if (height_range_slider_) {
        bool skip_slider_sync =
            height_range_slider_->defer_commit_until_unfocus() && height_range_slider_->has_pending_values();
        if (!skip_slider_sync) {
            height_range_slider_->set_min_value(state_->height_min);
            height_range_slider_->set_max_value(state_->height_max);
        }
    }

    if (changed) {
        const bool include_tags = !room_metadata_only_mode_;
        nlohmann::json canonical_metadata = nlohmann::json::object();
        state_->apply_to_json(canonical_metadata, allow_height, false);
        nlohmann::json new_snapshot = build_metadata_snapshot_json(canonical_metadata, room_tags_, room_anti_tags_, include_tags);
        std::size_t new_snapshot_hash = hash_metadata_snapshot(new_snapshot);
        if (new_snapshot_hash != metadata_snapshot_hash_) {
            if (!loaded_json_.is_object()) {
                loaded_json_ = nlohmann::json::object();
            }
            state_->apply_to_json(loaded_json_, allow_height, false);
            if (include_tags) {
                write_tags_to_json(loaded_json_);
            }

            if (room_ || external_room_json_) {
                auto& root = live_room_json();
                state_->apply_to_json(root, allow_height, false);
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
    if (last_screen_w_ > 0 && last_screen_h_ > 0) {
        prepare_for_event(last_screen_w_, last_screen_h_);
    }
    return container_->handle_event(e);
}

void RoomConfigurator::render(SDL_Renderer* r) const {
    if (!container_ || !container_->is_visible()) return;
    container_->render(r, last_screen_w_, last_screen_h_);
    DMDropdown::render_active_options(r);
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
        const bool allow_height = !is_trail_context_ && kTrailsAllowIndependentDimensions;
        copy.ensure_valid(allow_height);
        copy.apply_to_json(result, allow_height, !room_metadata_only_mode_);
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
        if (data.contains("tags")) {
            const auto& section = data["tags"];
            if (section.is_object()) {
                if (section.contains("include")) read_array(section["include"], include);
                if (section.contains("tags")) read_array(section["tags"], include);
                if (section.contains("exclude")) read_array(section["exclude"], exclude);
                if (section.contains("anti_tags")) read_array(section["anti_tags"], exclude);
            } else if (section.is_array()) {
                read_array(section, include);
            }
        }
        if (data.contains("anti_tags")) {
            read_array(data["anti_tags"], exclude);
        }
    }

    room_tags_.assign(include.begin(), include.end());
    room_anti_tags_.assign(exclude.begin(), exclude.end());
}

void RoomConfigurator::write_tags_to_json(nlohmann::json& object) const {
    if (!object.is_object()) {
        object = nlohmann::json::object();
    }
    if (room_tags_.empty() && room_anti_tags_.empty()) {
        object.erase("tags");
        object.erase("anti_tags");
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
