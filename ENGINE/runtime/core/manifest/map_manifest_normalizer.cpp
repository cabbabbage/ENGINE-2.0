#include "core/manifest/map_manifest_normalizer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "gameplay/map_generation/map_layers_geometry.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/weighted_range.hpp"

namespace manifest {
namespace {

constexpr int kDefaultSpawnRadius = 1500;
constexpr int kMinRoomDimension = 1;
constexpr int kMaxRoomDimension = 40000;
constexpr int kDefaultRoomMinDimension = 500;
constexpr int kDefaultRoomMaxDimension = 40000;
constexpr double kDefaultTrailSectorDirectionDeg = 0.0;
constexpr int kDefaultTrailSectorWidthPercent = 100;
constexpr int kMinTrailSectorWidthPercent = 25;
constexpr int kMaxTrailSectorWidthPercent = 100;
constexpr double kDegreesFullRotation = 360.0;

bool json_to_int(const nlohmann::json& value, int& out) {
    if (value.is_number_integer()) {
        out = value.get<int>();
        return true;
    }
    if (value.is_number_float()) {
        out = static_cast<int>(std::lround(value.get<double>()));
        return true;
    }
    return false;
}

bool json_to_string(const nlohmann::json& value, std::string& out) {
    if (!value.is_string()) {
        return false;
    }
    out = value.get<std::string>();
    return true;
}

bool json_to_double(const nlohmann::json& value, double& out) {
    if (value.is_number_float()) {
        out = value.get<double>();
        return true;
    }
    if (value.is_number_integer()) {
        out = static_cast<double>(value.get<int>());
        return true;
    }
    return false;
}

vibble::weighted_range::WeightedIntRange read_weighted_range_field(const nlohmann::json& entry,
                                                                   const char* key,
                                                                   const vibble::weighted_range::WeightedIntRange& fallback) {
    if (!entry.is_object() || !entry.contains(key)) {
        return fallback;
    }
    return vibble::weighted_range::from_json(entry.at(key), fallback);
}

vibble::weighted_range::WeightedIntRange read_weighted_range_legacy_pair(const nlohmann::json& entry,
                                                                         const char* min_key,
                                                                         const char* max_key,
                                                                         const vibble::weighted_range::WeightedIntRange& fallback) {
    if (!entry.is_object()) {
        return fallback;
    }
    bool has_min = false;
    bool has_max = false;
    std::int64_t min_value = fallback.center;
    std::int64_t max_value = fallback.center;
    if (entry.contains(min_key)) {
        if (entry[min_key].is_number_integer()) {
            min_value = entry[min_key].get<std::int64_t>();
            has_min = true;
        } else if (entry[min_key].is_number_float()) {
            min_value = static_cast<std::int64_t>(std::llround(entry[min_key].get<double>()));
            has_min = true;
        }
    }
    if (entry.contains(max_key)) {
        if (entry[max_key].is_number_integer()) {
            max_value = entry[max_key].get<std::int64_t>();
            has_max = true;
        } else if (entry[max_key].is_number_float()) {
            max_value = static_cast<std::int64_t>(std::llround(entry[max_key].get<double>()));
            has_max = true;
        }
    }
    if (!has_min && !has_max) {
        return fallback;
    }
    if (!has_min) {
        min_value = max_value;
    }
    if (!has_max) {
        max_value = min_value;
    }
    return vibble::weighted_range::make_legacy_uniform(min_value, max_value);
}

double normalize_direction_degrees(double value) {
    if (!std::isfinite(value)) {
        return kDefaultTrailSectorDirectionDeg;
    }
    double normalized = std::fmod(value, kDegreesFullRotation);
    if (normalized < 0.0) {
        normalized += kDegreesFullRotation;
    }
    if (normalized >= kDegreesFullRotation) {
        normalized -= kDegreesFullRotation;
    }
    return normalized;
}

bool normalize_trail_connection_sector(nlohmann::json& entry, bool include_for_room_entries) {
    bool changed = false;

    if (!include_for_room_entries) {
        if (entry.erase("trail_connection_sector") > 0) {
            changed = true;
        }
        return changed;
    }

    nlohmann::json sector = nlohmann::json::object();
    auto sector_it = entry.find("trail_connection_sector");
    if (sector_it != entry.end() && sector_it->is_object()) {
        sector = *sector_it;
    }

    double direction_deg = kDefaultTrailSectorDirectionDeg;
    if (sector.contains("direction_deg")) {
        (void)json_to_double(sector["direction_deg"], direction_deg);
    }
    direction_deg = normalize_direction_degrees(direction_deg);

    int width_percent = kDefaultTrailSectorWidthPercent;
    if (sector.contains("width_percent")) {
        (void)json_to_int(sector["width_percent"], width_percent);
    }
    width_percent = std::clamp(width_percent, kMinTrailSectorWidthPercent, kMaxTrailSectorWidthPercent);

    const bool has_sector_object = entry.contains("trail_connection_sector") && entry["trail_connection_sector"].is_object();
    const bool has_direction =
        has_sector_object &&
        entry["trail_connection_sector"].contains("direction_deg") &&
        (entry["trail_connection_sector"]["direction_deg"].is_number_float() ||
         entry["trail_connection_sector"]["direction_deg"].is_number_integer());
    const bool has_width =
        has_sector_object &&
        entry["trail_connection_sector"].contains("width_percent") &&
        entry["trail_connection_sector"]["width_percent"].is_number_integer();

    if (!has_sector_object || !has_direction || !has_width) {
        changed = true;
    } else {
        const double existing_direction = normalize_direction_degrees(entry["trail_connection_sector"]["direction_deg"].get<double>());
        const int existing_width = std::clamp(entry["trail_connection_sector"]["width_percent"].get<int>(),
                                              kMinTrailSectorWidthPercent,
                                              kMaxTrailSectorWidthPercent);
        if (std::abs(existing_direction - direction_deg) > 1e-6 || existing_width != width_percent) {
            changed = true;
        }
    }

    entry["trail_connection_sector"] = nlohmann::json::object({
        {"direction_deg", direction_deg},
        {"width_percent", width_percent},
    });
    return changed;
}

void sanitize_dimension_pair(int& min_value, int& max_value) {
    if (min_value <= 0 && max_value > 0) {
        min_value = max_value;
    } else if (max_value <= 0 && min_value > 0) {
        max_value = min_value;
    } else if (min_value <= 0 && max_value <= 0) {
        min_value = kDefaultRoomMinDimension;
        max_value = kDefaultRoomMaxDimension;
    }

    min_value = std::clamp(min_value, kMinRoomDimension, kMaxRoomDimension);
    max_value = std::clamp(max_value, kMinRoomDimension, kMaxRoomDimension);
    if (max_value < min_value) {
        std::swap(min_value, max_value);
    }
}

bool geometry_is_circle(const std::string& geometry) {
    std::string lowered;
    lowered.reserve(geometry.size());
    for (char ch : geometry) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered == "circle";
}

bool normalize_room_config_entry(nlohmann::json& entry, const std::string& key_name, bool include_trail_connection_sector) {
    bool changed = false;
    if (!entry.is_object()) {
        entry = nlohmann::json::object();
        changed = true;
    }

    if (!entry.contains("name") || !entry["name"].is_string() || entry["name"].get<std::string>().empty()) {
        entry["name"] = key_name;
        changed = true;
    }

    std::string geometry = "Square";
    const bool has_geometry_string = entry.contains("geometry") && json_to_string(entry["geometry"], geometry);
    if (!has_geometry_string || geometry.empty()) {
        geometry = "Square";
        entry["geometry"] = geometry;
        changed = true;
    }

    const auto default_range = vibble::weighted_range::make_legacy_uniform(kDefaultRoomMinDimension, kDefaultRoomMaxDimension);
    vibble::weighted_range::WeightedIntRange width_range = read_weighted_range_field(entry, "width", default_range);
    if (!entry.contains("width")) {
        if (entry.contains("min_width") || entry.contains("max_width")) {
            width_range = read_weighted_range_legacy_pair(entry, "min_width", "max_width", default_range);
        } else if (entry.contains("min_radius") || entry.contains("max_radius") || entry.contains("radius")) {
            width_range = read_weighted_range_legacy_pair(entry, "min_radius", "max_radius", default_range);
        }
    }

    vibble::weighted_range::WeightedIntRange height_range = read_weighted_range_field(entry, "height", width_range);
    if (!entry.contains("height") && (entry.contains("min_height") || entry.contains("max_height"))) {
        height_range = read_weighted_range_legacy_pair(entry, "min_height", "max_height", width_range);
    }

    if (geometry_is_circle(geometry)) {
        if (entry.contains("min_radius") || entry.contains("max_radius") || entry.contains("radius")) {
            width_range = read_weighted_range_legacy_pair(entry, "min_radius", "max_radius", default_range);
        }
        height_range = width_range;
    }

    const nlohmann::json width_json = vibble::weighted_range::to_json(width_range);
    const nlohmann::json height_json = vibble::weighted_range::to_json(height_range);
    if (!entry.contains("width") || entry["width"] != width_json) {
        entry["width"] = width_json;
        changed = true;
    }
    if (!entry.contains("height") || entry["height"] != height_json) {
        entry["height"] = height_json;
        changed = true;
    }

    if (entry.erase("min_width") > 0) changed = true;
    if (entry.erase("max_width") > 0) changed = true;
    if (entry.erase("min_height") > 0) changed = true;
    if (entry.erase("max_height") > 0) changed = true;
    if (entry.erase("radius") > 0) changed = true;
    if (entry.erase("min_radius") > 0) changed = true;
    if (entry.erase("max_radius") > 0) changed = true;

    auto normalize_bool = [&](const char* key, bool fallback) {
        const bool value = entry.contains(key) && entry[key].is_boolean() ? entry[key].get<bool>() : fallback;
        if (!entry.contains(key) || !entry[key].is_boolean() || entry[key].get<bool>() != value) {
            entry[key] = value;
            changed = true;
        }
    };
    normalize_bool("is_boss", false);
    const bool inherits_live_dynamic =
        entry.contains("inherits_live_dynamic_assets") && entry["inherits_live_dynamic_assets"].is_boolean()
            ? entry["inherits_live_dynamic_assets"].get<bool>()
            : (entry.contains("inherits_map_assets") && entry["inherits_map_assets"].is_boolean()
                ? entry["inherits_map_assets"].get<bool>()
                : false);
    if (!entry.contains("inherits_live_dynamic_assets") ||
        !entry["inherits_live_dynamic_assets"].is_boolean() ||
        entry["inherits_live_dynamic_assets"].get<bool>() != inherits_live_dynamic) {
        entry["inherits_live_dynamic_assets"] = inherits_live_dynamic;
        changed = true;
    }
    if (entry.erase("inherits_map_assets") > 0) {
        changed = true;
    }
    normalize_bool("inherit_map_floor_color", true);
    if (entry.erase("is_spawn") > 0) {
        changed = true;
    }

    auto normalize_rgb_color_array = [&](const char* key, int fallback_r, int fallback_g, int fallback_b) {
        std::array<int, 3> channels{
            fallback_r,
            fallback_g,
            fallback_b};
        bool valid = false;
        auto it = entry.find(key);
        if (it != entry.end() && it->is_array() && (it->size() == 3 || it->size() == 4)) {
            valid = true;
            for (std::size_t i = 0; i < 3; ++i) {
                int value = channels[i];
                if (!json_to_int((*it)[i], value)) {
                    valid = false;
                    break;
                }
                channels[i] = std::clamp(value, 0, 255);
            }
        }
        const nlohmann::json normalized = nlohmann::json::array({channels[0], channels[1], channels[2]});
        if (!valid || !entry.contains(key) || entry[key] != normalized) {
            entry[key] = normalized;
            changed = true;
        }
    };
    normalize_rgb_color_array("room_floor_color", 0, 0, 0);

    int edge_smoothness = 2;
    if (entry.contains("edge_smoothness")) {
        (void)json_to_int(entry["edge_smoothness"], edge_smoothness);
    }
    edge_smoothness = std::clamp(edge_smoothness, 0, 101);
    const bool has_numeric_edge = entry.contains("edge_smoothness") && json_to_int(entry["edge_smoothness"], existing_value);
    if (!has_numeric_edge || existing_value != edge_smoothness) {
        entry["edge_smoothness"] = edge_smoothness;
        changed = true;
    }

    const auto curvy_fallback = vibble::weighted_range::make_flat(2);
    vibble::weighted_range::WeightedIntRange curvyness_range = read_weighted_range_field(entry, "curvyness", curvy_fallback);
    if (!entry.contains("curvyness")) {
        if (entry.contains("curviness")) {
            curvyness_range = read_weighted_range_field(entry, "curviness", curvy_fallback);
        }
    }
    const nlohmann::json curvy_json = vibble::weighted_range::to_json(curvyness_range);
    if (!entry.contains("curvyness") || entry["curvyness"] != curvy_json) {
        entry["curvyness"] = curvy_json;
        changed = true;
    }
    if (entry.erase("curviness") > 0) {
        changed = true;
    }

    if (!entry.contains("spawn_groups") || !entry["spawn_groups"].is_array()) {
        entry["spawn_groups"] = nlohmann::json::array();
        changed = true;
    }

    if (normalize_trail_connection_sector(entry, include_trail_connection_sector)) {
        changed = true;
    }

    return changed;
}

bool normalize_room_config_section(nlohmann::json& section, bool include_trail_connection_sector) {
    if (!section.is_object()) {
        section = nlohmann::json::object();
        return true;
    }

    bool changed = false;
    for (auto it = section.begin(); it != section.end(); ++it) {
        if (normalize_room_config_entry(it.value(), it.key(), include_trail_connection_sector)) {
            changed = true;
        }
    }
    return changed;
}

bool ensure_object_section(nlohmann::json& root, const char* key) {
    auto it = root.find(key);
    if (it == root.end() || !it->is_object()) {
        root[key] = nlohmann::json::object();
        return true;
    }
    return false;
}

bool ensure_map_layers_settings_defaults(nlohmann::json& root) {
    bool changed = false;
    if (ensure_object_section(root, "map_layers_settings")) {
        changed = true;
    }

    nlohmann::json& settings = root["map_layers_settings"];
    const double normalized_min_edge = map_layers::min_edge_distance_from_map_manifest(root);
    auto min_edge_it = settings.find("min_edge_distance");
    if (min_edge_it == settings.end() ||
        !(min_edge_it->is_number_integer() || min_edge_it->is_number_float())) {
        settings["min_edge_distance"] = normalized_min_edge;
        return true;
    }

    const double current_value = min_edge_it->get<double>();
    if (!std::isfinite(current_value) || current_value != normalized_min_edge) {
        settings["min_edge_distance"] = normalized_min_edge;
        changed = true;
    }

    return changed;
}

nlohmann::json make_default_spawn_room(const std::string& spawn_name) {
    const int diameter = kDefaultSpawnRadius * 2;
    nlohmann::json entry = nlohmann::json::object();
    entry["name"] = spawn_name;
    entry["geometry"] = "Circle";
    entry["min_width"] = diameter;
    entry["max_width"] = diameter;
    entry["min_height"] = diameter;
    entry["max_height"] = diameter;
    entry["edge_smoothness"] = 2;
    entry["is_boss"] = false;
    entry["inherits_live_dynamic_assets"] = false;
    entry["inherit_map_floor_color"] = true;
    entry["room_floor_color"] = nlohmann::json::array({0, 0, 0});
    entry["spawn_groups"] = nlohmann::json::array();
    entry["trail_connection_sector"] = nlohmann::json::object({
        {"direction_deg", kDefaultTrailSectorDirectionDeg},
        {"width_percent", kDefaultTrailSectorWidthPercent},
    });
    return entry;
}

nlohmann::json make_room_spawn_group(const std::string& map_name,
                                     const std::string& display_name,
                                     const std::string& asset_name) {
    const int diameter = kDefaultSpawnRadius * 2;
    std::string cleaned = map_name;
    for (char& ch : cleaned) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }

    nlohmann::json group = nlohmann::json::object();
    group["display_name"] = display_name;
    group["spawn_id"] = std::string("spn-") + cleaned + "-" + display_name;
    group["position"] = "Exact";
    group["priority"] = 0;
    group["dx"] = 0;
    group["dz"] = 0;
    group["enforce_spacing"] = false;
    group["explicit_flip"] = false;
    group["force_flipped"] = false;
    group["locked"] = false;
    group["min_number"] = 1;
    group["max_number"] = 1;
    group["origional_height"] = diameter;
    group["origional_width"] = diameter;
    group["resolution"] = 6;
    group["resolve_geometry_to_room_size"] = true;
    group["resolve_quantity_to_room_size"] = false;
    group["candidates"] = nlohmann::json::array({
        nlohmann::json::object({{"name", "null"}, {"chance", 0}}),
        nlohmann::json::object({{"name", asset_name}, {"chance", 100}})
    });
    return group;
}

nlohmann::json make_batch_spawn_group(const std::string& map_name,
                                      const std::string& suffix,
                                      const std::string& display_name) {
    std::string cleaned = map_name;
    for (char& ch : cleaned) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }

    nlohmann::json group = nlohmann::json::object();
    group["display_name"] = display_name;
    group["spawn_id"] = std::string("spn-") + cleaned + "-" + suffix;
    group["position"] = "Random";
    group["execution_mode"] = "batch_grid";
    group["priority"] = 0;
    group["min_number"] = 0;
    group["max_number"] = 0;
    group["enforce_spacing"] = false;
    group["grid_resolution"] = 6;
    group["jitter"] = 0;
    group["resolution"] = 0;
    group["resolve_geometry_to_room_size"] = false;
    group["resolve_quantity_to_room_size"] = false;
    group["candidates"] = nlohmann::json::array({
        nlohmann::json::object({{"name", "null"}, {"chance", 100}})
    });
    return group;
}

void normalize_layer_candidate_entry(nlohmann::json& candidate, bool& changed) {
    if (!candidate.is_object()) {
        candidate = nlohmann::json::object();
        changed = true;
    }

    std::string source_type = candidate.value("source_type", std::string());
    if (source_type != "room_name" && source_type != "room_tag") {
        source_type = "room_name";
        changed = true;
    }

    std::string value = candidate.value("value", std::string());
    if (value.empty()) {
        if (candidate.contains("name") && candidate["name"].is_string()) {
            value = candidate["name"].get<std::string>();
            changed = true;
        }
    }

    int min_instances = 0;
    if (candidate.contains("min_instances") && candidate["min_instances"].is_number_integer()) {
        min_instances = std::max(0, candidate["min_instances"].get<int>());
    }
    int max_instances = 1;
    if (candidate.contains("max_instances") && candidate["max_instances"].is_number_integer()) {
        max_instances = std::max(0, candidate["max_instances"].get<int>());
    }
    if (max_instances < min_instances) {
        max_instances = min_instances;
        changed = true;
    }

    if (!candidate.contains("required_children") || !candidate["required_children"].is_array()) {
        candidate["required_children"] = nlohmann::json::array();
        changed = true;
    }

    candidate["source_type"] = source_type;
    candidate["value"] = value;
    candidate["name"] = value; // compatibility mirror
    candidate["min_instances"] = min_instances;
    candidate["max_instances"] = max_instances;
}

bool ensure_layer_zero_defaults(nlohmann::json& map_manifest, const std::string& map_id) {
    bool changed = false;
    nlohmann::json& rooms_data = map_manifest["rooms_data"];
    if (!rooms_data.is_object()) {
        rooms_data = nlohmann::json::object();
        changed = true;
    }

    if (!map_manifest.contains("map_layers") || !map_manifest["map_layers"].is_array()) {
        map_manifest["map_layers"] = nlohmann::json::array();
        changed = true;
    }
    nlohmann::json& layers = map_manifest["map_layers"];
    const bool map_was_empty = layers.empty() && rooms_data.empty();
    if (layers.empty()) {
        layers.push_back(nlohmann::json::object());
        changed = true;
    }

    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i].is_object()) {
            layers[i] = nlohmann::json::object();
            changed = true;
        }
        layers[i]["level"] = static_cast<int>(i);
        if (!layers[i].contains("name") || !layers[i]["name"].is_string()) {
            layers[i]["name"] = std::string("layer_") + std::to_string(i);
            changed = true;
        }
        if (!layers[i].contains("rooms") || !layers[i]["rooms"].is_array()) {
            layers[i]["rooms"] = nlohmann::json::array();
            changed = true;
        }
    }

    nlohmann::json& layer0 = layers[0];
    nlohmann::json& layer0_rooms = layer0["rooms"];
    for (auto& candidate : layer0_rooms) {
        normalize_layer_candidate_entry(candidate, changed);
    }

    if (map_was_empty) {
        nlohmann::json spawn_candidate = nlohmann::json::object({
            {"source_type", "room_name"},
            {"value", std::string("Spawn")},
            {"name", std::string("Spawn")},
            {"min_instances", 1},
            {"max_instances", 1},
            {"required_children", nlohmann::json::array()}
        });
        layer0_rooms = nlohmann::json::array({std::move(spawn_candidate)});
        changed = true;
        if (!rooms_data.contains("Spawn") || !rooms_data["Spawn"].is_object()) {
            rooms_data["Spawn"] = make_default_spawn_room("Spawn");
            changed = true;
        }
    }
    if (!layer0_rooms.empty()) {
        normalize_layer_candidate_entry(layer0_rooms[0], changed);
        layer0_rooms[0]["min_instances"] = 1;
        layer0_rooms[0]["max_instances"] = 1;
        if (layer0_rooms.size() > 1) {
            layer0_rooms.erase(layer0_rooms.begin() + 1, layer0_rooms.end());
            changed = true;
        }
        layer0["min_rooms"] = 1;
        layer0["max_rooms"] = 1;
    } else {
        layer0["min_rooms"] = 0;
        layer0["max_rooms"] = 0;
    }

    for (auto& room_entry : rooms_data.items()) {
        if (room_entry.value().is_object()) {
            if (room_entry.value().erase("is_spawn") > 0) {
                changed = true;
            }
            if (!room_entry.value().contains("room_tags") || !room_entry.value()["room_tags"].is_array()) {
                room_entry.value()["room_tags"] = nlohmann::json::array();
                changed = true;
            }
        }
    }

    if (!map_id.empty()) {
        layer0["name"] = layer0.value("name", std::string("layer_0"));
    }

    return changed;
}

struct AssetNameCanonicalLookup {
    std::unordered_map<std::string, std::string> unique_by_normalized_name;
    std::unordered_set<std::string> ambiguous_normalized_names;
};

std::string normalize_asset_lookup_token(const std::string& raw_name) {
    std::string normalized;
    normalized.reserve(raw_name.size());
    for (unsigned char ch : raw_name) {
        if (std::isalnum(ch) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    constexpr const char* kTypo = "backround";
    constexpr const char* kCorrect = "background";
    std::string::size_type pos = 0;
    while ((pos = normalized.find(kTypo, pos)) != std::string::npos) {
        normalized.replace(pos, std::char_traits<char>::length(kTypo), kCorrect);
        pos += std::char_traits<char>::length(kCorrect);
    }
    return normalized;
}

AssetNameCanonicalLookup build_asset_name_lookup(const nlohmann::json* asset_catalog) {
    AssetNameCanonicalLookup lookup;
    if (!asset_catalog || !asset_catalog->is_object()) {
        return lookup;
    }

    for (auto it = asset_catalog->begin(); it != asset_catalog->end(); ++it) {
        const std::string canonical_name = it.key();
        if (canonical_name.empty()) {
            continue;
        }
        const std::string normalized = normalize_asset_lookup_token(canonical_name);
        if (normalized.empty()) {
            continue;
        }

        if (lookup.ambiguous_normalized_names.find(normalized) != lookup.ambiguous_normalized_names.end()) {
            continue;
        }

        const auto existing = lookup.unique_by_normalized_name.find(normalized);
        if (existing == lookup.unique_by_normalized_name.end()) {
            lookup.unique_by_normalized_name.emplace(normalized, canonical_name);
            continue;
        }

        if (existing->second != canonical_name) {
            lookup.unique_by_normalized_name.erase(existing);
            lookup.ambiguous_normalized_names.insert(normalized);
        }
    }

    return lookup;
}

bool normalize_candidate_asset_name(nlohmann::json& candidate,
                                    const AssetNameCanonicalLookup& lookup) {
    if (!candidate.is_object()) {
        return false;
    }
    auto name_it = candidate.find("name");
    if (name_it == candidate.end() || !name_it->is_string()) {
        return false;
    }

    const std::string current_name = name_it->get<std::string>();
    if (current_name.empty()) {
        return false;
    }

    std::string current_lower = current_name;
    std::transform(current_lower.begin(), current_lower.end(), current_lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (current_lower == "null") {
        return false;
    }

    const std::string normalized = normalize_asset_lookup_token(current_name);
    if (normalized.empty()) {
        return false;
    }

    if (lookup.ambiguous_normalized_names.find(normalized) != lookup.ambiguous_normalized_names.end()) {
        return false;
    }

    const auto canonical_it = lookup.unique_by_normalized_name.find(normalized);
    if (canonical_it == lookup.unique_by_normalized_name.end()) {
        return false;
    }

    if (canonical_it->second == current_name) {
        return false;
    }

    *name_it = canonical_it->second;
    return true;
}

bool normalize_spawn_group_candidates(nlohmann::json& group,
                                      const AssetNameCanonicalLookup& lookup) {
    if (!group.is_object()) {
        return false;
    }

    auto candidates_it = group.find("candidates");
    if (candidates_it == group.end() || !candidates_it->is_array()) {
        return false;
    }

    bool changed = false;
    for (auto& candidate : *candidates_it) {
        if (normalize_candidate_asset_name(candidate, lookup)) {
            changed = true;
        }
    }
    return changed;
}

bool normalize_spawn_group_array(nlohmann::json& owner,
                                 const char* key,
                                 const AssetNameCanonicalLookup& lookup) {
    if (!owner.is_object()) {
        return false;
    }

    auto groups_it = owner.find(key);
    if (groups_it == owner.end() || !groups_it->is_array()) {
        return false;
    }

    bool changed = false;
    for (auto& group : *groups_it) {
        if (normalize_spawn_group_candidates(group, lookup)) {
            changed = true;
        }
    }
    return changed;
}

bool normalize_map_manifest_asset_ids(nlohmann::json& map_manifest,
                                      const nlohmann::json* asset_catalog) {
    const AssetNameCanonicalLookup lookup = build_asset_name_lookup(asset_catalog);
    if (lookup.unique_by_normalized_name.empty()) {
        return false;
    }

    bool changed = false;

    if (normalize_spawn_group_array(map_manifest, "candidate_selectors", lookup)) {
        changed = true;
    }

    if (map_manifest.contains("live_dynamic_spawns") && map_manifest["live_dynamic_spawns"].is_object()) {
        nlohmann::json& live_dynamic_spawns = map_manifest["live_dynamic_spawns"];
        if (normalize_spawn_group_array(live_dynamic_spawns, "boundary_area_selectors", lookup)) {
            changed = true;
        }
        if (normalize_spawn_group_array(live_dynamic_spawns, "inherited_map_selectors", lookup)) {
            changed = true;
        }
    }

    auto normalize_room_like_section = [&](const char* section_name) {
        auto section_it = map_manifest.find(section_name);
        if (section_it == map_manifest.end() || !section_it->is_object()) {
            return;
        }

        for (auto section_entry_it = section_it->begin(); section_entry_it != section_it->end(); ++section_entry_it) {
            if (!section_entry_it.value().is_object()) {
                continue;
            }
            if (normalize_spawn_group_array(section_entry_it.value(), "spawn_groups", lookup)) {
                changed = true;
            }
            if (normalize_spawn_group_array(section_entry_it.value(), "candidate_selectors", lookup)) {
                changed = true;
            }
        }
    };

    normalize_room_like_section("rooms_data");
    normalize_room_like_section("trails_data");
    return changed;
}

} // namespace

nlohmann::json build_default_map_manifest(const std::string& map_name) {
    const int diameter = kDefaultSpawnRadius * 2;

    nlohmann::json map_info = nlohmann::json::object();

    nlohmann::json layer = nlohmann::json::object();
    layer["name"] = "layer_0";
    layer["level"] = 0;
    layer["min_rooms"] = 1;
    layer["max_rooms"] = 1;
    nlohmann::json spawn_spec = nlohmann::json::object();
    spawn_spec["source_type"] = "room_name";
    spawn_spec["value"] = "Spawn";
    spawn_spec["name"] = "Spawn";
    spawn_spec["min_instances"] = 1;
    spawn_spec["max_instances"] = 1;
    spawn_spec["required_children"] = nlohmann::json::array();
    layer["rooms"] = nlohmann::json::array({spawn_spec});
    map_info["map_layers"] = nlohmann::json::array({layer});

    map_info["live_dynamic_spawns"] = nlohmann::json::object({
        {"boundary_area_selectors",
         nlohmann::json::array({make_batch_spawn_group(map_name, "map_boundary", "batch_map_boundary")})}
    });
    map_info["trails_data"] = nlohmann::json::object({
        {"basic", nlohmann::json::object({
            {"name", "basic"},
            {"display_color", nlohmann::json::array({85, 242, 143, 255})},
            {"edge_smoothness", 2},
            {"geometry", "Line"},
            {"inherits_live_dynamic_assets", false},
            {"is_boss", false},
            {"width", vibble::weighted_range::to_json(vibble::weighted_range::make_legacy_uniform(400, 800))},
            {"height", vibble::weighted_range::to_json(vibble::weighted_range::make_legacy_uniform(400, 800))},
            {"curvyness", vibble::weighted_range::to_json(vibble::weighted_range::make_flat(2))},
            {"spawn_groups", nlohmann::json::array()}
        })}
    });

    map_info["map_layers_settings"] = nlohmann::json::object({{"min_edge_distance", 200}});

    nlohmann::json spawn_room = nlohmann::json::object();
    spawn_room["name"] = "Spawn";
    spawn_room["geometry"] = "Circle";
    spawn_room["width"] = vibble::weighted_range::to_json(vibble::weighted_range::make_flat(diameter));
    spawn_room["height"] = vibble::weighted_range::to_json(vibble::weighted_range::make_flat(diameter));
    spawn_room["edge_smoothness"] = 2;
    spawn_room["curvyness"] = vibble::weighted_range::to_json(vibble::weighted_range::make_flat(2));
    spawn_room["is_boss"] = false;
    spawn_room["inherits_live_dynamic_assets"] = true;
    spawn_room["trail_connection_sector"] = nlohmann::json::object({
        {"direction_deg", kDefaultTrailSectorDirectionDeg},
        {"width_percent", kDefaultTrailSectorWidthPercent},
    });
    spawn_room["display_color"] = nlohmann::json::array({120, 170, 235, 255});
    spawn_room["areas"] = nlohmann::json::array({
        nlohmann::json::object({
            {"name", "spawn_center"},
            {"type", "spawning"},
            {"kind", "Spawn"},
            {"resolution", 3},
            {"points", nlohmann::json::array({
                nlohmann::json::object({{"x", -256}, {"y", -256}}),
                nlohmann::json::object({{"x", 256}, {"y", -256}}),
                nlohmann::json::object({{"x", 256}, {"y", 256}}),
                nlohmann::json::object({{"x", -256}, {"y", 256}})
            })}
        })
    });
    spawn_room["spawn_groups"] = nlohmann::json::array({
        make_room_spawn_group(map_name, "Vibble", "Vibble")
    });
    spawn_room["room_tags"] = nlohmann::json::array();

    map_info["rooms_data"] = nlohmann::json::object();
    map_info["rooms_data"]["Spawn"] = std::move(spawn_room);
    // Camera runtime defaults:
    // - transition_damping: dt-stable response speed (1/seconds)
    // - max_camera_velocity: world-pixel velocity cap to prevent jitter/overshoot
    // - room_blend_damping_scale/room_blend_velocity_scale: slow room-to-room transitions
    // - room_blend_follow_weight_scale: reduce player-follow pull during room blending
    // - settle_duration_after_stop: post-stop carry time before returning to Idle
    // - movement_look_ahead_weight: optional movement look-ahead contribution
    // - player_follow_weight: blend room target toward player to keep follow tighter
    // - player_soft_leash_px/player_hard_leash_px: keep player from drifting to screen edge
    map_info["camera_settings"] = nlohmann::json::object({
        {"smooth_motion_height", true},
        {"base_height_px", 720.0},
        {"min_visible_screen_ratio", 0.01},
        {"transition_damping", 9.0},
        {"max_camera_velocity", 2200.0},
        {"room_blend_damping_scale", 0.14},
        {"room_blend_velocity_scale", 0.18},
        {"room_blend_follow_weight_scale", 0.28},
        {"settle_duration_after_stop", 0.20},
        {"movement_look_ahead_weight", 0.12},
        {"player_follow_weight", 0.35},
        {"player_soft_leash_px", 220.0},
        {"player_hard_leash_px", 360.0}
    });
    map_info["map_grid_settings"] = nlohmann::json::object({{"grid_resolution", 8}});
    map_info["dev_map_settings"] = nlohmann::json::object({
        {"default_floor_color", nlohmann::json::array({0, 0, 0})},
    });
    map_info["audio"] = nlohmann::json::object({
        {"music", nlohmann::json::object({
            {"content_root", (std::filesystem::path("content") / map_name / "music").generic_string()},
            {"tracks", nlohmann::json::array()}
        })}
    });
    map_info["map_name"] = map_name;
    map_info["content_root"] = (std::filesystem::path("content") / map_name).generic_string();

    return map_info;
}

MapManifestNormalizationResult normalize_map_manifest(nlohmann::json map_manifest,
                                                      const std::string& map_id,
                                                      const std::filesystem::path& manifest_root,
                                                      const nlohmann::json* asset_catalog) {
    MapManifestNormalizationResult result;
    bool changed = false;

    if (!map_manifest.is_object()) {
        map_manifest = nlohmann::json::object();
        changed = true;
    }

    const std::array<const char*, 3> object_sections{
        "live_dynamic_spawns",
        "rooms_data",
        "trails_data"
    };
    for (const char* key : object_sections) {
        if (ensure_object_section(map_manifest, key)) {
            changed = true;
        }
    }
    if (map_manifest.erase("map_assets_data") > 0) {
        changed = true;
    }
    if (map_manifest.contains("candidate_selectors") && map_manifest["candidate_selectors"].is_array()) {
        nlohmann::json& live = map_manifest["live_dynamic_spawns"];
        if (!live.is_object()) {
            live = nlohmann::json::object();
        }
        const bool missing_inherited =
            !live.contains("inherited_map_selectors") ||
            !live["inherited_map_selectors"].is_array() ||
            live["inherited_map_selectors"].empty();
        if (missing_inherited) {
            live["inherited_map_selectors"] = map_manifest["candidate_selectors"];
        }
        map_manifest.erase("candidate_selectors");
        changed = true;
    }
    if (map_manifest.contains("map_boundary_data") && map_manifest["map_boundary_data"].is_object()) {
        nlohmann::json& live = map_manifest["live_dynamic_spawns"];
        if (!live.is_object()) {
            live = nlohmann::json::object();
        }
        const bool missing_boundary =
            !live.contains("boundary_area_selectors") ||
            !live["boundary_area_selectors"].is_array() ||
            live["boundary_area_selectors"].empty();
        if (missing_boundary) {
            const nlohmann::json& legacy = map_manifest["map_boundary_data"];
            auto candidates = legacy.find("candidate_selectors");
            if (candidates != legacy.end() && candidates->is_array()) {
                live["boundary_area_selectors"] = *candidates;
                changed = true;
            }
        }
        map_manifest.erase("map_boundary_data");
        changed = true;
    }

    if (ensure_map_layers_settings_defaults(map_manifest)) {
        changed = true;
    }

    const bool had_grid_section =
        map_manifest.contains("map_grid_settings") &&
        map_manifest["map_grid_settings"].is_object();
    const nlohmann::json grid_before = had_grid_section
        ? map_manifest["map_grid_settings"]
        : nlohmann::json();
    ensure_map_grid_settings(map_manifest);
    if (!had_grid_section || map_manifest["map_grid_settings"] != grid_before) {
        changed = true;
    }

    if (ensure_layer_zero_defaults(map_manifest, map_id)) {
        changed = true;
    }
    auto normalize_map_default_floor_color = [&]() {
        if (!map_manifest.contains("dev_map_settings") || !map_manifest["dev_map_settings"].is_object()) {
            map_manifest["dev_map_settings"] = nlohmann::json::object();
            changed = true;
        }
        nlohmann::json& dev = map_manifest["dev_map_settings"];
        auto color_it = dev.find("default_floor_color");
        if (color_it == dev.end()) {
            color_it = dev.find("map_color");
        }
        std::array<int, 3> channels{0, 0, 0};
        bool valid = false;
        if (color_it != dev.end() && color_it->is_array() && (color_it->size() == 3 || color_it->size() == 4)) {
            valid = true;
            for (std::size_t i = 0; i < 3; ++i) {
                int value = channels[i];
                if (!json_to_int((*color_it)[i], value)) {
                    valid = false;
                    break;
                }
                channels[i] = std::clamp(value, 0, 255);
            }
        }
        nlohmann::json normalized = nlohmann::json::array({channels[0], channels[1], channels[2]});
        if (!valid || !dev.contains("default_floor_color") || dev["default_floor_color"] != normalized) {
            dev["default_floor_color"] = normalized;
            changed = true;
        }
    };
    normalize_map_default_floor_color();
    if (normalize_map_manifest_asset_ids(map_manifest, asset_catalog)) {
        changed = true;
    }
    if (normalize_room_config_section(map_manifest["rooms_data"], true)) {
        changed = true;
    }
    if (normalize_room_config_section(map_manifest["trails_data"], false)) {
        changed = true;
    }
    if (!map_manifest.contains("map_name") ||
        !map_manifest["map_name"].is_string() ||
        map_manifest["map_name"].get<std::string>().empty()) {
        map_manifest["map_name"] = map_id;
        changed = true;
    }

    std::filesystem::path relative_content_root;
    auto root_it = map_manifest.find("content_root");
    if (root_it != map_manifest.end() && root_it->is_string()) {
        const std::string& value = root_it->get_ref<const std::string&>();
        if (!value.empty()) {
            relative_content_root = std::filesystem::path(value);
        }
    }
    if (relative_content_root.empty()) {
        relative_content_root = std::filesystem::path("content") / map_id;
        map_manifest["content_root"] = relative_content_root.generic_string();
        changed = true;
    }

    std::filesystem::path resolved_root = relative_content_root;
    if (resolved_root.is_relative()) {
        resolved_root = manifest_root / resolved_root;
    }
    resolved_root = resolved_root.lexically_normal();

    result.map_manifest = std::move(map_manifest);
    result.resolved_content_root = std::move(resolved_root);
    result.changed = changed;
    return result;
}

MapManifestBootstrapResult bootstrap_map_manifest(const ManifestData& manifest_data,
                                                  const std::string& map_id) {
    MapManifestBootstrapResult bootstrap;

    nlohmann::json map_manifest_json = nlohmann::json::object();
    bool manifest_entry_found = false;
    if (manifest_data.maps.is_object()) {
        auto map_it = manifest_data.maps.find(map_id);
        if (map_it != manifest_data.maps.end() && map_it.value().is_object()) {
            map_manifest_json = map_it.value();
            manifest_entry_found = true;
        }
    }

    const std::filesystem::path manifest_root =
        std::filesystem::path(manifest::manifest_path()).parent_path();
    MapManifestNormalizationResult normalized = normalize_map_manifest(std::move(map_manifest_json),
                                                                       map_id,
                                                                       manifest_root,
                                                                       &manifest_data.assets);

    bootstrap.map_manifest = std::move(normalized.map_manifest);
    bootstrap.resolved_content_root = std::move(normalized.resolved_content_root);
    bootstrap.manifest_entry_found = manifest_entry_found;
    bootstrap.changed = normalized.changed || !manifest_entry_found;
    return bootstrap;
}

}
