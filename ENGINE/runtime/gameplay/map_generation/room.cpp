#include "room.hpp"
#include "core/axis_convention.hpp"
#include "gameplay/spawn/asset_spawner.hpp"
#include "assets/asset/asset_types.hpp"
#include "devtools/core/manifest_store.hpp"
#include "room_manifest_adapter.hpp"
#include "room_legacy_migration.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <iostream>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include "utils/grid.hpp"
#include "utils/string_utils.hpp"
#include "utils/ranged_color.hpp"
#include "rendering/render/camera_controller.hpp"
using json = nlohmann::json;

namespace {

using vibble::strings::to_lower_copy;

constexpr int kDefaultRoomSize = 9;
constexpr int kMinRoomSize = 7;
constexpr int kMaxRoomSize = 20;

RoomAreaSerialization::Kind parse_kind_value(const std::string& value) {
        if (value.empty()) return RoomAreaSerialization::Kind::Unknown;
        std::string lowered = to_lower_copy(value);
        if (lowered.find("spawn") != std::string::npos) {
                return RoomAreaSerialization::Kind::Spawn;
        }
        if (lowered.find("trigger") != std::string::npos) {
                return RoomAreaSerialization::Kind::Trigger;
        }
        return RoomAreaSerialization::Kind::Unknown;
}

SDL_Point min_corner_anchor(const std::vector<SDL_Point>& points) {
        if (points.empty()) return SDL_Point{0, 0};
        SDL_Point anchor{points.front().x, points.front().y};
        for (const auto& p : points) {
                anchor.x = std::min(anchor.x, p.x);
                anchor.y = std::min(anchor.y, p.y);
        }
        return anchor;
}

}

namespace RoomAreaSerialization {

Kind infer_kind_from_strings(const std::string& kind_value,
                             const std::string& type_hint,
                             const std::string& name_hint) {
        if (Kind parsed = parse_kind_value(kind_value); parsed != Kind::Unknown) {
                return parsed;
        }
        if (Kind parsed = parse_kind_value(type_hint); parsed != Kind::Unknown) {
                return parsed;
        }
        if (Kind parsed = parse_kind_value(name_hint); parsed != Kind::Unknown) {
                return parsed;
        }
        return Kind::Unknown;
}

Kind infer_kind_from_entry(const nlohmann::json& entry,
                           const std::string& type_hint,
                           const std::string& name_hint) {
        std::string provided;
        if (entry.contains("kind") && entry["kind"].is_string()) {
                provided = entry["kind"].get<std::string>();
        }
        return infer_kind_from_strings(provided, type_hint, name_hint);
}

std::string to_string(Kind kind) {
        switch (kind) {
        case Kind::Spawn:   return "Spawn";
        case Kind::Trigger: return "Trigger";
        case Kind::Unknown: default: return std::string{};
        }
}

bool is_supported_kind(Kind kind) {
        return kind == Kind::Spawn || kind == Kind::Trigger;
}

AnchorData resolve_anchor(const nlohmann::json& entry,
                          axis::WorldPos default_anchor,
                          Kind kind) {
        AnchorData data;
        data.world = default_anchor;
        data.relative_offset = SDL_Point{0, 0};
        data.relative_height_offset = 0;
        data.relative_to_center = is_supported_kind(kind);

        axis::WorldPos stored{};
        bool has_anchor = false;
        if (entry.contains("anchor") && entry["anchor"].is_object()) {
                stored.x = entry["anchor"].value("x", default_anchor.x);
                stored.y = entry["anchor"].value("y", default_anchor.y);
                stored.z = entry["anchor"].value("z", default_anchor.z);
                has_anchor = true;
        }

        bool has_flag = entry.contains("anchor_relative_to_center");
        bool wants_relative = data.relative_to_center;
        if (has_flag && entry["anchor_relative_to_center"].is_boolean()) {
                wants_relative = entry["anchor_relative_to_center"].get<bool>();
        } else if (!has_flag && data.relative_to_center) {

                stored = axis::WorldPos{0, 0, 0};
                wants_relative = true;
        }

        if (wants_relative && data.relative_to_center) {
                data.relative_offset = SDL_Point{stored.x, stored.z};
                data.relative_height_offset = stored.y;
                data.world.x = default_anchor.x + stored.x;
                data.world.y = default_anchor.y + stored.y;
                data.world.z = default_anchor.z + stored.z;
                data.relative_to_center = true;
        } else if (has_anchor) {
                data.world = stored;
                data.relative_offset.x = stored.x - default_anchor.x;
                data.relative_offset.y = stored.z - default_anchor.z;
                data.relative_height_offset = stored.y - default_anchor.y;
                data.relative_to_center = false;
        } else {
                data.relative_offset = SDL_Point{0, 0};
                data.relative_height_offset = 0;
                data.world = default_anchor;
        }

        return data;
}

void write_anchor(nlohmann::json& entry,
                  const AnchorData& anchor,
                  Kind kind) {
        if (is_supported_kind(kind) && anchor.relative_to_center) {
                entry["anchor"] = nlohmann::json::object({
                        {"x", anchor.relative_offset.x},
                        {"y", anchor.relative_height_offset},
                        {"z", anchor.relative_offset.y}
                });
                entry["anchor_relative_to_center"] = true;
        } else {
                entry["anchor"] = nlohmann::json::object({
                        {"x", anchor.world.x},
                        {"y", anchor.world.y},
                        {"z", anchor.world.z}
                });
                entry.erase("anchor_relative_to_center");
        }
}

SDL_Point choose_anchor(Kind kind,
                        SDL_Point default_anchor,
                        const std::vector<SDL_Point>& world_points) {
        if (!world_points.empty() && !is_supported_kind(kind)) {
                return min_corner_anchor(world_points);
        }
        return default_anchor;
}

std::vector<SDL_Point> decode_relative_points(const nlohmann::json& entry) {
        std::vector<SDL_Point> pts;
        if (!entry.contains("points") || !entry["points"].is_array()) {
                return pts;
        }
        pts.reserve(entry["points"].size());
        for (const auto& point : entry["points"]) {
                if (!point.is_object()) continue;
                int x = point.value("x", 0);
                int z = point.value("z", 0);
                pts.push_back(SDL_Point{x, z});
        }
        return pts;
}

int clamp_i64_to_int(std::int64_t value) {
        if (value < static_cast<std::int64_t>(std::numeric_limits<int>::min())) {
                return std::numeric_limits<int>::min();
        }
        if (value > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
                return std::numeric_limits<int>::max();
        }
        return static_cast<int>(value);
}

std::pair<std::int64_t, std::int64_t> expected_relative_limits(const nlohmann::json& entry) {
        int width = 0;
        int height = 0;
        if (entry.contains("origin_room") && entry["origin_room"].is_object()) {
                const auto& origin = entry["origin_room"];
                width = origin.value("width", 0);
                height = origin.value("height", 0);
        }
        if (width <= 0) {
                width = entry.value("origional_width", 0);
        }
        if (height <= 0) {
                height = entry.value("origional_height", 0);
        }
        constexpr std::int64_t kMinLimit = 2048;
        constexpr std::int64_t kMaxLimit = (1LL << 20);
        std::int64_t limit_x = std::max<std::int64_t>(kMinLimit, static_cast<std::int64_t>(width) * 4LL);
        std::int64_t limit_z = std::max<std::int64_t>(kMinLimit, static_cast<std::int64_t>(height) * 4LL);
        limit_x = std::clamp(limit_x, kMinLimit, kMaxLimit);
        limit_z = std::clamp(limit_z, kMinLimit, kMaxLimit);
        return {limit_x, limit_z};
}

double correction_score(const std::vector<SDL_Point>& points,
                        axis::WorldPos anchor,
                        int multiplier,
                        std::int64_t limit_x,
                        std::int64_t limit_z) {
        std::int64_t max_abs_x = 0;
        std::int64_t max_abs_z = 0;
        std::int64_t sum_abs_x = 0;
        std::int64_t sum_abs_z = 0;
        for (const auto& p : points) {
                const std::int64_t rel_x = static_cast<std::int64_t>(p.x) -
                                           static_cast<std::int64_t>(multiplier) * static_cast<std::int64_t>(anchor.x);
                const std::int64_t rel_z = static_cast<std::int64_t>(p.y) -
                                           static_cast<std::int64_t>(multiplier) * static_cast<std::int64_t>(anchor.z);
                const std::int64_t abs_x = std::llabs(rel_x);
                const std::int64_t abs_z = std::llabs(rel_z);
                max_abs_x = std::max(max_abs_x, abs_x);
                max_abs_z = std::max(max_abs_z, abs_z);
                sum_abs_x += abs_x;
                sum_abs_z += abs_z;
        }

        const double mean_abs =
            static_cast<double>(sum_abs_x + sum_abs_z) / static_cast<double>(std::max<std::size_t>(points.size(), 1));
        double score = static_cast<double>(max_abs_x + max_abs_z) + mean_abs * 0.1;
        if (max_abs_x > limit_x) {
                score += static_cast<double>(max_abs_x - limit_x) * 20.0;
        }
        if (max_abs_z > limit_z) {
                score += static_cast<double>(max_abs_z - limit_z) * 20.0;
        }
        score += static_cast<double>(std::abs(multiplier)) * 128.0;
        return score;
}

int choose_anchor_drift_multiplier(const std::vector<SDL_Point>& points,
                                   const nlohmann::json& entry,
                                   axis::WorldPos anchor) {
        if (points.empty()) {
                return 0;
        }
        if (anchor.x == 0 && anchor.z == 0) {
                return 0;
        }

        double mean_x = 0.0;
        double mean_z = 0.0;
        for (const auto& p : points) {
                mean_x += static_cast<double>(p.x);
                mean_z += static_cast<double>(p.y);
        }
        const double inv_count = 1.0 / static_cast<double>(points.size());
        mean_x *= inv_count;
        mean_z *= inv_count;

        std::vector<int> candidates{0};
        auto append_guesses = [&](int anchor_component, double mean_component) {
                if (anchor_component == 0) {
                        return;
                }
                const int base = static_cast<int>(std::llround(mean_component / static_cast<double>(anchor_component)));
                for (int delta = -2; delta <= 2; ++delta) {
                        candidates.push_back(base + delta);
                }
        };
        append_guesses(anchor.x, mean_x);
        append_guesses(anchor.z, mean_z);
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        const auto [limit_x, limit_z] = expected_relative_limits(entry);
        const double baseline = correction_score(points, anchor, 0, limit_x, limit_z);
        int best_multiplier = 0;
        double best_score = baseline;
        for (int multiplier : candidates) {
                const double score = correction_score(points, anchor, multiplier, limit_x, limit_z);
                if (score < best_score) {
                        best_score = score;
                        best_multiplier = multiplier;
                }
        }

        if (best_multiplier == 0) {
                return 0;
        }
        const bool substantially_better =
            (best_score <= baseline * 0.75) || ((baseline - best_score) >= 2048.0);
        return substantially_better ? best_multiplier : 0;
}

void normalize_relative_points_for_anchor(std::vector<SDL_Point>& points,
                                          const nlohmann::json& entry,
                                          axis::WorldPos anchor) {
        const int multiplier = choose_anchor_drift_multiplier(points, entry, anchor);
        if (multiplier == 0) {
                return;
        }
        const std::int64_t shift_x =
            static_cast<std::int64_t>(multiplier) * static_cast<std::int64_t>(anchor.x);
        const std::int64_t shift_z =
            static_cast<std::int64_t>(multiplier) * static_cast<std::int64_t>(anchor.z);
        for (auto& p : points) {
                p.x = clamp_i64_to_int(static_cast<std::int64_t>(p.x) - shift_x);
                p.y = clamp_i64_to_int(static_cast<std::int64_t>(p.y) - shift_z);
        }
        std::cerr << "[Room] Corrected area point anchor drift with multiplier "
                  << multiplier << " (anchor=(" << anchor.x << "," << anchor.z << "))\n";
}

std::vector<SDL_Point> decode_points(const nlohmann::json& entry, axis::WorldPos anchor) {
        std::vector<SDL_Point> pts;
        auto rel = decode_relative_points(entry);
        normalize_relative_points_for_anchor(rel, entry, anchor);
        pts.reserve(rel.size());
        for (const auto& point : rel) {
                pts.push_back(SDL_Point{anchor.x + point.x, anchor.z + point.y});
        }
        return pts;
}

nlohmann::json encode_points(const std::vector<SDL_Point>& points, axis::WorldPos anchor) {
        nlohmann::json arr = nlohmann::json::array();
        arr.get_ref<nlohmann::json::array_t&>().reserve(points.size());
        for (const auto& p : points) {
                const std::int64_t rel_x =
                    static_cast<std::int64_t>(p.x) - static_cast<std::int64_t>(anchor.x);
                const std::int64_t rel_z =
                    static_cast<std::int64_t>(p.y) - static_cast<std::int64_t>(anchor.z);
                arr.push_back({ {"x", clamp_i64_to_int(rel_x)},
                                {"y", anchor.y},
                                {"z", clamp_i64_to_int(rel_z)} });
        }
        return arr;
}

}
Room::Room(Point origin,
           std::string type_,
           const std::string& room_def_name,
           Room* parent,
           const std::string& manifest_context,
           AssetLibrary* asset_lib,
           Area* precomputed_area,
           nlohmann::json* room_data,
           const MapGridSettings& grid_settings,
           double map_radius,
           const std::string& data_section,
           nlohmann::json* map_info_root,
           devmode::core::ManifestStore* manifest_store,
           std::string manifest_map_id,
           Room::ManifestWriter manifest_writer,
           bool auto_populate_assets)
    : map_origin(origin),
      parent(parent),
      room_name(room_def_name),
      room_directory(manifest_context.empty() ? data_section : manifest_context + "::" + data_section),
      json_path((manifest_context.empty() ? data_section : manifest_context + "::" + data_section) + "::" + room_def_name),
      room_area(nullptr),
      type(type_),
      room_data_ptr_(room_data),
      map_grid_settings_(grid_settings),
      manifest_context_(manifest_context),
      data_section_(data_section),
      manifest_writer_(std::move(manifest_writer)) {
    if (testing) {
        std::cout << "[Room] Created room: " << room_name
                  << " at (" << origin.first << ", " << origin.second << ")"
                  << (parent ? " with parent\n" : " (no parent)\n");
    }

    if (!manifest_map_id.empty()) {
        manifest_map_id_ = std::move(manifest_map_id);
    }
    manifest_store_ = manifest_store;
    map_info_root_ = map_info_root;
    if (manifest_store_ || map_info_root_ || manifest_writer_) {
        sync_boundary_ = std::make_unique<RoomManifestAdapter::EditorSyncBoundary>();
    } else {
        sync_boundary_ = std::make_unique<RoomManifestAdapter::RuntimeSyncBoundary>();
    }

    if (room_data_ptr_) {
        if (room_data_ptr_->is_null()) {
            *room_data_ptr_ = json::object();
        }
        if (room_data_ptr_->is_object()) {
            assets_json = *room_data_ptr_;
        }
    }
    if (!assets_json.is_object()) {
        assets_json = json::object();
    }

    inherits_map_assets_ = assets_json.value(
        "inherits_live_dynamic_assets",
        assets_json.value("inherits_map_assets", false));

    const nlohmann::json* map_camera_settings = nullptr;
    if (map_info_root_ && map_info_root_->is_object()) {
        auto it = map_info_root_->find("camera_settings");
        if (it != map_info_root_->end() && it->is_object()) {
            map_camera_settings = &(*it);
        }
    }

    auto read_number = [](const nlohmann::json* src, const char* key, double fallback) {
        if (!src || !src->is_object()) {
            return fallback;
        }
        auto it = src->find(key);
        if (it == src->end() || !it->is_number()) {
            return fallback;
        }
        const double value = it->get<double>();
        if (!std::isfinite(value)) {
            return fallback;
        }
        return value;
    };

    const int default_camera_height =
        std::clamp(static_cast<int>(std::lround(read_number(map_camera_settings, "camera_height_px", 1000.0))), 1, 2000);
    const float default_camera_tilt = camera_math::sanitize_pitch_degrees(
        static_cast<float>(read_number(map_camera_settings, "camera_tilt_deg", 60.0)));

    auto read_room_int = [&](const char* key, int fallback) {
        return static_cast<int>(std::lround(read_number(&assets_json, key, static_cast<double>(fallback))));
    };
    auto read_room_float = [&](const char* key, float fallback) {
        return static_cast<float>(read_number(&assets_json, key, static_cast<double>(fallback)));
    };

    camera_height_px = std::clamp(read_room_int("camera_height_px", default_camera_height), 1, 2000);
    camera_tilt_deg = camera_math::sanitize_pitch_degrees(
        read_room_float("camera_tilt_deg", default_camera_tilt));
    camera_zoom_percent = std::clamp(read_room_int("camera_zoom_percent", 0), 0, 100);
    camera_center_dx = read_room_int("camera_center_dx", 0);
    camera_center_dz = read_room_int("camera_center_dz", 0);

    load_named_areas_from_json();

    int map_radius_int = static_cast<int>(std::round(map_radius));
    if (map_radius_int < 0) {
        map_radius_int = 0;
    }
    const int map_w = map_radius_int * 2;
    const int map_h = map_radius_int * 2;

    if (precomputed_area) {
        if (testing) {
            std::cout << "[Room] Using precomputed area for: " << room_name << "\n";
        }
        room_area = std::make_unique<Area>(room_name, precomputed_area->get_points(), 3);
        if (room_area) {
            room_area->set_type(type.empty() ? "room" : type);
        }
    } else {
        const auto size_info = room_legacy_migration::resolve_room_size(
            assets_json,
            kDefaultRoomSize,
            [this](const char* reason) {
                std::cout << "[Room] Legacy room migration executed for '" << room_name
                          << "' reason=" << (reason ? reason : "unknown") << "\n";
            });
        const int size = std::clamp(size_info.size, kMinRoomSize, kMaxRoomSize);

        if (testing) {
            std::cout << "[Room] Creating area from JSON: " << room_name
                      << " (size=" << size << ")"
                      << " at (" << map_origin.first << ", " << map_origin.second << ")"
                      << ", map radius: " << map_radius << "\n";
        }

        room_area = std::make_unique<Area>(
            room_name,
            SDL_Point{map_origin.first, map_origin.second},
            size,
            map_w,
            map_h,
            size);

        if (room_area) {
            room_area->set_type("room");
        }
    }

    if (!auto_populate_assets || !asset_lib || !room_area) {
        return;
    }

    std::vector<json> json_sources;
    std::vector<AssetSpawnPlanner::SourceContext> source_contexts;

    auto push_payload = [this](const std::function<void(nlohmann::json&)>& mutate) {
        if (!mutate) {
            return;
        }

        if (map_info_root_) {
            if (!map_info_root_->is_object()) {
                *map_info_root_ = nlohmann::json::object();
            }
            mutate(*map_info_root_);
        }

        auto apply_mutation = [&](nlohmann::json payload) {
            if (!payload.is_object()) {
                payload = nlohmann::json::object();
            }
            mutate(payload);
            return payload;
        };

        nlohmann::json payload = map_info_root_ ? *map_info_root_ : nlohmann::json::object();
        payload = apply_mutation(std::move(payload));
        RoomManifestAdapter::SyncContext ctx;
        ctx.data_section = data_section_;
        ctx.room_name = room_name;
        ctx.manifest_map_id = manifest_map_id_;
        ctx.manifest_store = manifest_store_;
        ctx.map_info_root = map_info_root_;
        ctx.manifest_writer = manifest_writer_;
        RoomManifestAdapter::write_payload(ctx, payload, "Room::push_payload");
    };

    json_sources.push_back(assets_json);

    AssetSpawnPlanner::SourceContext room_context;
    room_context.persist = [this, push_payload](const nlohmann::json& updated) {
        assets_json = updated;
        if (room_data_ptr_) {
            *room_data_ptr_ = assets_json;
        }
        push_payload([&](nlohmann::json& payload) {
            nlohmann::json& section = payload[data_section_];
            if (!section.is_object()) {
                section = nlohmann::json::object();
            }
            section[room_name] = assets_json;
        });
    };
    source_contexts.push_back(room_context);

    planner = std::make_unique<AssetSpawnPlanner>(json_sources, *room_area, *asset_lib, source_contexts);

    std::vector<Area> exclusion;
    AssetSpawner spawner(asset_lib, exclusion);
    spawner.spawn(*this);
}

Room::~Room() = default;

void Room::set_sibling_left(Room* left_room) {
	left_sibling = left_room;
}

void Room::set_sibling_right(Room* right_room) {
	right_sibling = right_room;
}

void Room::add_connecting_room(Room* room) {
	if (room && std::find(connected_rooms.begin(), connected_rooms.end(), room) == connected_rooms.end()) {
		connected_rooms.push_back(room);
	}
}

void Room::remove_connecting_room(Room* room) {
	auto it = std::find(connected_rooms.begin(), connected_rooms.end(), room);
	if (it != connected_rooms.end()) connected_rooms.erase(it);
}

void Room::add_room_assets(std::vector<std::unique_ptr<Asset>> new_assets) {
	for (auto& asset : new_assets)
	assets.push_back(std::move(asset));
}

std::vector<std::unique_ptr<Asset>>&& Room::get_room_assets() {
	return std::move(assets);
}

void Room::set_scale(double s) {
	if (s <= 0.0) s = 1.0;
	scale_ = s;
}

int Room::clamp_int(int v, int lo, int hi) const {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

void Room::bounds_to_size(const std::tuple<int,int,int,int>& b, int& w, int& h) const {
        int minx, miny, maxx, maxy;
        std::tie(minx, miny, maxx, maxy) = b;
        w = std::max(0, maxx - minx);
        h = std::max(0, maxy - miny);
}

std::pair<int, int> Room::current_room_dimensions() const {
        if (room_area) {
                int w = 0;
                int h = 0;
                bounds_to_size(room_area->get_bounds(), w, h);
                return {w, h};
        }

        const auto size_info = room_legacy_migration::resolve_room_size(assets_json, kDefaultRoomSize, nullptr);
        const int size = std::clamp(size_info.size, kMinRoomSize, kMaxRoomSize);
        const int radius = size * vibble::grid::delta(size);
        return {radius * 2, radius * 2};
}

void Room::load_named_areas_from_json() {
        areas.clear();
        try {
                if (!assets_json.is_object()) return;
                if (!assets_json.contains("areas") || !assets_json["areas"].is_array()) return;

                axis::WorldPos default_anchor{};
                if (room_area) {
                        auto center = room_area->get_center();
                        default_anchor = axis::WorldPos{center.x, 0, center.y};
                } else {
                        default_anchor = axis::WorldPos{map_origin.first, 0, map_origin.second};
                }

                auto room_dims = current_room_dimensions();

                for (auto& item : assets_json["areas"]) {
                        if (!item.is_object()) continue;
                        const std::string name = item.value("name", std::string{});
                        if (name.empty()) continue;
                        const std::string type = item.value("type", std::string{});

                        RoomAreaSerialization::Kind kind =
                                RoomAreaSerialization::infer_kind_from_entry(item, type, name);
                        if (!RoomAreaSerialization::is_supported_kind(kind)) {
                                std::cerr << "[Room] Ignoring area '" << name << "' with unsupported kind '"
                                          << item.value("kind", std::string{}) << "'. Rooms support Spawn/Trigger only.\n";
                                continue;
                        }

                        auto anchor = RoomAreaSerialization::resolve_anchor(item, default_anchor, kind);

                        const int resolution = vibble::grid::clamp_resolution(item.value("resolution", 2));
                        bool scale_to_room = item.value("scale_to_room", false);
                        const int stored_width = item.value("origional_width", 0);
                        const int stored_height = item.value("origional_height", 0);

                        // Relative points are manifest-owned serialized coordinates.
                        // They are normalized into runtime-owned `areas` snapshots below.
                        auto relative_points = RoomAreaSerialization::decode_relative_points(item);

                        std::vector<SDL_Point> pts;
                        const int current_width = room_dims.first;
                        const int current_height = room_dims.second;
                        const bool can_scale = scale_to_room && stored_width > 0 && stored_height > 0 &&
                                               current_width > 0 && current_height > 0;
                        int persisted_width = stored_width;
                        int persisted_height = stored_height;

                        auto scale_component = [](int value, double factor) {
                                return static_cast<int>(std::llround(static_cast<double>(value) * factor));
};

                        if (can_scale) {
                                RoomAreaSerialization::normalize_relative_points_for_anchor(relative_points, item, anchor.world);
                                const double sx = static_cast<double>(current_width) / static_cast<double>(stored_width);
                                const double sy = static_cast<double>(current_height) / static_cast<double>(stored_height);

                                if (anchor.relative_to_center) {
                                        anchor.relative_offset.x = scale_component(anchor.relative_offset.x, sx);
                                        anchor.relative_offset.y = scale_component(anchor.relative_offset.y, sy);
                                        anchor.world.x = default_anchor.x + anchor.relative_offset.x;
                                        anchor.world.z = default_anchor.z + anchor.relative_offset.y;
                                        anchor.world.y = default_anchor.y + anchor.relative_height_offset;
                                }

                                pts.reserve(relative_points.size());
                                for (const auto& rel : relative_points) {
                                        const int dx = scale_component(rel.x, sx);
                                        const int dz = scale_component(rel.y, sy);
                                        pts.push_back(SDL_Point{anchor.world.x + dx, anchor.world.z + dz});
                                }
                                persisted_width = current_width;
                                persisted_height = current_height;
                        } else {
                                pts = RoomAreaSerialization::decode_points(item, anchor.world);
                        }

                        if (pts.size() < 3) continue;

                        RoomAreaSerialization::write_anchor(item, anchor, kind);
                        item["points"] = RoomAreaSerialization::encode_points(pts, anchor.world);
                        item["resolution"] = resolution;
                        item.erase("relative_points");
                        item.erase("original_width");
                        item.erase("original_height");
                        if (scale_to_room) {
                                item["scale_to_room"] = true;
                                if (persisted_width > 0) item["origional_width"] = persisted_width;
                                if (persisted_height > 0) item["origional_height"] = persisted_height;
                        } else {
                                item.erase("scale_to_room");
                        }

                        NamedArea na;
                        na.name = name;
                        na.type = type;
                        na.kind = RoomAreaSerialization::to_string(kind);
                        na.area = std::make_unique<Area>(name, pts, resolution);
                        if (na.area) {
                                na.area->set_resolution(resolution);
                        }
                        if (na.area) na.area->set_type(type);
                        na.scale_to_room = scale_to_room;
                        na.original_room_width = persisted_width;
                        na.original_room_height = persisted_height;

                        try {
                                if (item.contains("origin_room") && item["origin_room"].is_object()) {
                                        const auto& orj = item["origin_room"];
                                        NamedArea::OriginRoomMeta meta;
                                        meta.name = orj.value("name", std::string{});
                                        meta.width = orj.value("width", 0);
                                        meta.height = orj.value("height", 0);
                                        if (orj.contains("anchor") && orj["anchor"].is_object()) {
                                                meta.anchor.x = orj["anchor"].value("x", 0);
                                                meta.anchor.y = orj["anchor"].value("y", 0);
                                                meta.anchor.z = orj["anchor"].value("z", 0);
                                        }
                                        meta.anchor_relative_to_center = orj.value("anchor_relative_to_center", false);
                                        na.origin_room = meta;
                                } else {

                                        nlohmann::json meta = nlohmann::json::object();
                                        meta["name"] = room_name;
                                        meta["width"] = room_dims.first;
                                        meta["height"] = room_dims.second;
                                        meta["anchor"] = nlohmann::json::object({
                                                {"x", anchor.world.x},
                                                {"y", anchor.world.y},
                                                {"z", anchor.world.z}
                                        });
                                        meta["anchor_relative_to_center"] = anchor.relative_to_center;
                                        item["origin_room"] = meta;
                                        NamedArea::OriginRoomMeta store;
                                        store.name = room_name;
                                        store.width = room_dims.first;
                                        store.height = room_dims.second;
                                        store.anchor = anchor.world;
                                        store.anchor_relative_to_center = anchor.relative_to_center;
                                        na.origin_room = store;
                                }
                        } catch (...) {

                        }
                        areas.push_back(std::move(na));
                }
        } catch (...) {

        }
}

Area* Room::find_area(const std::string& name) {
        if (name.empty()) return nullptr;
        for (auto& na : areas) {
                if (na.name == name && na.area) return na.area.get();
        }
        return nullptr;
}

bool Room::remove_area(const std::string& name) {
        if (name.empty()) {
                return false;
        }
        bool removed = false;
        try {
                if (assets_json.is_object() && assets_json.contains("areas") && assets_json["areas"].is_array()) {
                        auto& arr = assets_json["areas"];
                        for (auto it = arr.begin(); it != arr.end();) {
                                if (it->is_object() && it->value("name", std::string{}) == name) {
                                        it = arr.erase(it);
                                        removed = true;
                                } else {
                                        ++it;
                                }
                        }
                }
        } catch (...) {
                removed = false;
        }
        if (removed) {
                load_named_areas_from_json();
        }
        return removed;
}

bool Room::rename_area(const std::string& old_name, const std::string& new_name) {
        if (old_name.empty() || new_name.empty()) {
                return false;
        }
        if (old_name == new_name) {
                return true;
        }
        for (const auto& na : areas) {
                if (na.name == new_name) {
                        return false;
                }
        }
        bool renamed = false;
        try {
                if (assets_json.is_object() && assets_json.contains("areas") && assets_json["areas"].is_array()) {
                        for (auto& entry : assets_json["areas"]) {
                                if (entry.is_object() && entry.value("name", std::string{}) == old_name) {
                                        entry["name"] = new_name;
                                        renamed = true;
                                }
                        }
                }
        } catch (...) {
                return false;
        }
        if (!renamed) {
                return false;
        }
        load_named_areas_from_json();
        return true;
}

void Room::upsert_named_area(const Area& area,
                             bool scale_to_room,
                             int original_room_width,
                             int original_room_height) {
        const std::string area_name = area.get_name();
        if (area_name.empty()) {
                return;
        }

        if (!assets_json.is_object()) {
                assets_json = nlohmann::json::object();
        }
        if (!assets_json.contains("areas") || !assets_json["areas"].is_array()) {
                assets_json["areas"] = nlohmann::json::array();
        }

        const auto& pts = area.get_points();
        if (pts.size() < 3) {
                return;
        }

        std::string effective_type = area.get_type();
        nlohmann::json* existing_entry = nullptr;
        std::string existing_kind;
        for (auto& item : assets_json["areas"]) {
                if (!item.is_object()) continue;
                if (item.value("name", std::string{}) == area_name) {
                        existing_entry = &item;
                        if (effective_type.empty()) {
                                effective_type = item.value("type", std::string{});
                        }
                        existing_kind = item.value("kind", std::string{});
                        break;
                }
        }

        RoomAreaSerialization::Kind kind =
                RoomAreaSerialization::infer_kind_from_strings(existing_kind, effective_type, area_name);
        if (!RoomAreaSerialization::is_supported_kind(kind)) {
                std::cerr << "[Room] Refusing to store area '" << area_name
                          << "' with unsupported kind (" << existing_kind << ").\n";
                return;
        }

        axis::WorldPos default_anchor{};
        if (room_area) {
                auto center = room_area->get_center();
                default_anchor = axis::WorldPos{center.x, 0, center.y};
        } else {
                default_anchor = axis::WorldPos{map_origin.first, 0, map_origin.second};
        }
        RoomAreaSerialization::AnchorData anchor;
        SDL_Point default_anchor_point{default_anchor.x, default_anchor.z};
        SDL_Point initial_anchor = RoomAreaSerialization::choose_anchor(kind, default_anchor_point, pts);
        anchor.world.x = initial_anchor.x;
        anchor.world.z = initial_anchor.y;
        anchor.world.y = default_anchor.y;
        anchor.relative_offset = SDL_Point{anchor.world.x - default_anchor.x,
                                           anchor.world.z - default_anchor.z};
        anchor.relative_height_offset = 0;
        anchor.relative_to_center = RoomAreaSerialization::is_supported_kind(kind);
        if (existing_entry) {
                anchor = RoomAreaSerialization::resolve_anchor(*existing_entry, default_anchor, kind);
        }

        bool final_scale_flag = scale_to_room;

        int stored_width = original_room_width;
        int stored_height = original_room_height;
        if (existing_entry) {
                if (stored_width <= 0) stored_width = existing_entry->value("origional_width", 0);
                if (stored_height <= 0) stored_height = existing_entry->value("origional_height", 0);
        }
        if (final_scale_flag) {
                auto dims = current_room_dimensions();
                if (stored_width <= 0) stored_width = dims.first;
                if (stored_height <= 0) stored_height = dims.second;
        }

        nlohmann::json entry = nlohmann::json::object({
                {"name", area_name},
                {"points", RoomAreaSerialization::encode_points(pts, anchor.world)},
        });
        if (!effective_type.empty()) {
                entry["type"] = effective_type;
        }
        entry["kind"] = RoomAreaSerialization::to_string(kind);
        RoomAreaSerialization::write_anchor(entry, anchor, kind);

        entry["resolution"] = vibble::grid::clamp_resolution(area.resolution());

        if (final_scale_flag) {
                entry["scale_to_room"] = true;
                if (stored_width > 0) entry["origional_width"] = stored_width;
                if (stored_height > 0) entry["origional_height"] = stored_height;
        }

        try {
                auto dims = current_room_dimensions();
                nlohmann::json origin_meta = nlohmann::json::object();
                origin_meta["name"] = room_name;
                origin_meta["width"] = std::max(0, dims.first);
                origin_meta["height"] = std::max(0, dims.second);
                origin_meta["anchor"] = nlohmann::json::object({
                        {"x", anchor.world.x},
                        {"y", anchor.world.y},
                        {"z", anchor.world.z}
                });
                origin_meta["anchor_relative_to_center"] = anchor.relative_to_center;
                entry["origin_room"] = std::move(origin_meta);
        } catch (...) {

        }

        if (existing_entry) {
                *existing_entry = entry;
        } else {
                assets_json["areas"].push_back(entry);
        }

        load_named_areas_from_json();
}

nlohmann::json Room::create_static_room_json(std::string name) {
        json out;
        const auto size_info = room_legacy_migration::resolve_room_size(assets_json, kDefaultRoomSize, nullptr);
        const int size = std::clamp(size_info.size, kMinRoomSize, kMaxRoomSize);
        int width = 0, height = 0;
        if (room_area) {
                bounds_to_size(room_area->get_bounds(), width, height);
        }
	out["name"] = std::move(name);
        out["size"] = size;
        if (assets_json.contains("coarseness")) {
                const auto& c = assets_json["coarseness"];
                int legacy_coarseness = 0;
                if (c.is_number_integer()) {
                        legacy_coarseness = c.get<int>();
                        const int clamped = std::clamp(legacy_coarseness, 0, 4000);
                        if (clamped <= 0) {
                                out["coarseness"] = vibble::weighted_range::to_json(vibble::weighted_range::make_flat(0));
                        } else {
                                const int min_radius = std::max(8, 12 + (clamped / 18));
                                const int max_radius = std::max(min_radius, 36 + (clamped / 4));
                                out["coarseness"] = vibble::weighted_range::to_json(
                                    vibble::weighted_range::make_legacy_uniform(min_radius, max_radius));
                        }
                } else if (c.is_number_float()) {
                        legacy_coarseness = static_cast<int>(std::lround(c.get<double>()));
                        const int clamped = std::clamp(legacy_coarseness, 0, 4000);
                        if (clamped <= 0) {
                                out["coarseness"] = vibble::weighted_range::to_json(vibble::weighted_range::make_flat(0));
                        } else {
                                const int min_radius = std::max(8, 12 + (clamped / 18));
                                const int max_radius = std::max(min_radius, 36 + (clamped / 4));
                                out["coarseness"] = vibble::weighted_range::to_json(
                                    vibble::weighted_range::make_legacy_uniform(min_radius, max_radius));
                        }
                } else {
                        out["coarseness"] = vibble::weighted_range::to_json(
                            vibble::weighted_range::from_json(c, vibble::weighted_range::make_flat(0)));
                }
        } else {
                out["coarseness"] = vibble::weighted_range::to_json(vibble::weighted_range::make_flat(0));
        }
        out.erase("radius");
        out.erase("min_radius");
        out.erase("max_radius");
        out.erase("min_width");
        out.erase("max_width");
        out.erase("min_height");
        out.erase("max_height");
        out.erase("geometry");
        out.erase("edge_detail_candidates");
	out["is_boss"] = assets_json.value("is_boss", false);
	out["inherits_live_dynamic_assets"] = assets_json.value(
            "inherits_live_dynamic_assets",
            assets_json.value("inherits_map_assets", false));
        out["inherit_map_floor_color"] = assets_json.value("inherit_map_floor_color", true);
        if (assets_json.contains("room_floor_color")) {
                out["room_floor_color"] = assets_json["room_floor_color"];
        } else {
                out["room_floor_color"] = nlohmann::json::array({0, 0, 0});
        }
        json spawn_groups = json::array();
        int cx = 0, cy = 0;
        if (room_area) {
                auto c = room_area->get_center();
                cx = c.x;
                cy = c.y;
	}
        for (const auto& uptr : assets) {
                const Asset* a = uptr.get();
                if (!a || !a->info) continue;

                const int ax = a->world_x();
                const int ay = a->world_y();
                json entry;
                entry["min_number"] = 1;
                entry["max_number"] = 1;
                entry["position"] = "Exact";
                entry["enforce_spacing"] = false;
                entry["dx"] = ax - cx;
                entry["dz"] = ay - cy;
                if (width > 0) entry["origional_width"] = width;
                if (height > 0) entry["origional_height"] = height;
                entry["display_name"] = a->info->name;
                entry["candidates"] = json::array();
                entry["candidates"].push_back({{"name", "null"}, {"chance", 0}});
                entry["candidates"].push_back({{"name", a->info->name}, {"chance", 100}});
                spawn_groups.push_back(std::move(entry));
        }
        out["spawn_groups"] = std::move(spawn_groups);
        return out;
}

nlohmann::json& Room::assets_data() {
        if (!assets_json.is_object()) {
                assets_json = nlohmann::json::object();
        }
        if (!assets_json.contains("spawn_groups") || !assets_json["spawn_groups"].is_array()) {
                assets_json["spawn_groups"] = nlohmann::json::array();
        }
        return assets_json;
}

const nlohmann::json& Room::assets_data() const {
        static const nlohmann::json kEmpty = nlohmann::json::object();
        if (!assets_json.is_object()) {
                return kEmpty;
        }
        return assets_json;
}

SDL_Color Room::display_color() const {
        static constexpr SDL_Color kFallback{120, 170, 235, 255};
        if (!assets_json.is_object()) {
                return kFallback;
        }
        auto it = assets_json.find("display_color");
        if (it == assets_json.end()) {
                return kFallback;
        }
        if (auto parsed = utils::color::color_from_json(*it)) {
                SDL_Color color = *parsed;
                color.a = 255;
                return color;
        }
        return kFallback;
}

bool Room::inherit_map_floor_color() const {
        if (!assets_json.is_object()) {
                return true;
        }
        auto it = assets_json.find("inherit_map_floor_color");
        if (it == assets_json.end() || !it->is_boolean()) {
                return true;
        }
        return it->get<bool>();
}

SDL_Color Room::room_floor_color(SDL_Color fallback) const {
        fallback.a = 255;
        if (!assets_json.is_object()) {
                return fallback;
        }
        auto it = assets_json.find("room_floor_color");
        if (it == assets_json.end()) {
                return fallback;
        }
        if (auto parsed = utils::color::color_from_json(*it)) {
                SDL_Color color = *parsed;
                color.a = 255;
                return color;
        }
        return fallback;
}

void Room::rename(const std::string& new_name, nlohmann::json& map_info_json) {
        std::string normalized = vibble::strings::to_lower_copy(new_name);
        if (normalized.empty() || normalized == room_name) {
                if (!room_data_ptr_ && map_info_json.is_object()) {
                        nlohmann::json& section = map_info_json[data_section_];
                        if (section.is_object() && section.contains(room_name)) {
                                room_data_ptr_ = &section[room_name];
                        }
                }
                return;
        }

        if (!map_info_json.is_object()) {
                map_info_json = nlohmann::json::object();
        }

        nlohmann::json& section = map_info_json[data_section_];
        if (!section.is_object()) {
                section = nlohmann::json::object();
        }

        if (room_data_ptr_) {
                assets_json = *room_data_ptr_;
        } else {
                auto it = section.find(room_name);
                if (it != section.end()) {
                        assets_json = *it;
                }
        }

        assets_json["name"] = normalized;

        section[normalized] = assets_json;
        nlohmann::json* new_entry = &section[normalized];

        if (section.contains(room_name)) {
                section.erase(room_name);
        }

        room_name = normalized;
        room_data_ptr_ = new_entry;
        assets_json = *room_data_ptr_;

        if (!json_path.empty()) {
                size_t pos = json_path.rfind("::");
                if (pos != std::string::npos) {
                        json_path = json_path.substr(0, pos + 2) + room_name;
                } else {
                        json_path = room_name;
                }
        }

        if (room_area) {
                room_area->set_name(room_name);
        }

        for (auto& owned : assets) {
                if (owned) {
                        owned->set_owning_room_name(room_name);
                }
        }
}

void Room::set_manifest_store(devmode::core::ManifestStore* store,
                              std::string map_id,
                              nlohmann::json* map_info_root,
                              Room::ManifestWriter manifest_writer) {
        manifest_store_ = store;
        manifest_map_id_ = std::move(map_id);
        map_info_root_ = map_info_root;
        if (manifest_writer) {
                manifest_writer_ = std::move(manifest_writer);
        }
        if (manifest_store_ || map_info_root_ || manifest_writer_) {
                sync_boundary_ = std::make_unique<RoomManifestAdapter::EditorSyncBoundary>();
        } else {
                sync_boundary_ = std::make_unique<RoomManifestAdapter::RuntimeSyncBoundary>();
        }
}

nlohmann::json Room::build_room_payload_for_save() const {
        assets_save_dirty_ = true;
        const_cast<Room*>(this)->load_named_areas_from_json();

        const nlohmann::json normalized = RoomManifestAdapter::normalize_room_snapshot(
                assets_json,
                camera_height_px,
                camera_tilt_deg,
                camera_zoom_percent,
                camera_center_dx,
                camera_center_dz);

        RoomManifestAdapter::SyncContext ctx;
        ctx.data_section = data_section_;
        ctx.room_name = room_name;
        ctx.manifest_map_id = manifest_map_id_;
        ctx.manifest_store = manifest_store_;
        ctx.map_info_root = map_info_root_;
        ctx.manifest_writer = manifest_writer_;
        return RoomManifestAdapter::build_payload_from_snapshot(ctx, normalized);
}

void Room::snapshot_assets_to_map_info() {
        if (!sync_boundary_ || !sync_boundary_->enabled()) {
                return;
        }
        // Keep the in-memory manifest representation aligned with the live room state
        // without letting stale runtime copies overwrite map-mode edits.
        if (!assets_json.is_object()) {
                assets_json = nlohmann::json::object();
        }

        if (map_info_root_) {
                if (!map_info_root_->is_object()) {
                        *map_info_root_ = nlohmann::json::object();
                }
                nlohmann::json& section = (*map_info_root_)[data_section_];
                if (!section.is_object()) {
                        section = nlohmann::json::object();
                }

                auto existing = section.find(room_name);
                const bool has_existing_entry =
                        existing != section.end() && existing->is_object();
                const bool should_write_runtime_state =
                        assets_save_dirty_ || !has_existing_entry;

                if (should_write_runtime_state) {
                        section[room_name] = assets_json;
                        room_data_ptr_ = &section[room_name];
                        assets_save_dirty_ = false;
                        return;
                }

                // Preserve externally edited room JSON (for example, map-mode panel edits)
                // when runtime room data is not marked dirty.
                assets_json = *existing;
                room_data_ptr_ = &(*existing);
                return;
        }

        if (room_data_ptr_) {
                if (assets_save_dirty_ || !room_data_ptr_->is_object()) {
                        *room_data_ptr_ = assets_json;
                } else {
                        assets_json = *room_data_ptr_;
                }
                assets_save_dirty_ = false;
        }
}

bool Room::apply_room_payload_for_save(const nlohmann::json& payload) const {
        if (!payload.is_object()) {
                return false;
        }
        if (room_data_ptr_) {
                *room_data_ptr_ = assets_json;
        }
        if (map_info_root_) {
                *map_info_root_ = payload;
        }

        RoomManifestAdapter::SyncContext ctx;
        ctx.data_section = data_section_;
        ctx.room_name = room_name;
        ctx.manifest_map_id = manifest_map_id_;
        ctx.manifest_store = manifest_store_;
        ctx.map_info_root = map_info_root_;
        ctx.manifest_writer = manifest_writer_;
        const bool success = RoomManifestAdapter::write_payload(ctx, payload, "Room::apply_room_payload_for_save");

        if (success) {
                assets_save_dirty_ = false;
        }
        return success;
}
