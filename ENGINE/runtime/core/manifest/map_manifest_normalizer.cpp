#include "core/manifest/map_manifest_normalizer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

#include "gameplay/map_generation/map_layers_geometry.hpp"
#include "utils/map_grid_settings.hpp"

namespace manifest {
namespace {

constexpr int kDefaultSpawnRadius = 1500;
constexpr int kMinRoomDimension = 1;
constexpr int kMaxRoomDimension = 4000;
constexpr int kDefaultRoomMinDimension = 500;
constexpr int kDefaultRoomMaxDimension = 4000;

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

bool normalize_room_config_entry(nlohmann::json& entry, const std::string& key_name) {
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

    int width_min = kDefaultRoomMinDimension;
    int width_max = kDefaultRoomMaxDimension;
    int height_min = kDefaultRoomMinDimension;
    int height_max = kDefaultRoomMaxDimension;
    bool has_width_min = false;
    bool has_width_max = false;
    bool has_height_min = false;
    bool has_height_max = false;

    if (entry.contains("min_width") && json_to_int(entry["min_width"], width_min)) {
        has_width_min = true;
    }
    if (entry.contains("max_width") && json_to_int(entry["max_width"], width_max)) {
        has_width_max = true;
    }
    if (entry.contains("min_height") && json_to_int(entry["min_height"], height_min)) {
        has_height_min = true;
    }
    if (entry.contains("max_height") && json_to_int(entry["max_height"], height_max)) {
        has_height_max = true;
    }

    int legacy_min_radius = 0;
    int legacy_max_radius = 0;
    bool has_legacy_min_radius = false;
    bool has_legacy_max_radius = false;
    if (entry.contains("min_radius") && json_to_int(entry["min_radius"], legacy_min_radius)) {
        legacy_min_radius = std::max(0, legacy_min_radius);
        has_legacy_min_radius = true;
    }
    if (entry.contains("max_radius") && json_to_int(entry["max_radius"], legacy_max_radius)) {
        legacy_max_radius = std::max(0, legacy_max_radius);
        has_legacy_max_radius = true;
    }
    int legacy_radius = 0;
    if (entry.contains("radius") && json_to_int(entry["radius"], legacy_radius)) {
        legacy_radius = std::max(0, legacy_radius);
        if (!has_legacy_min_radius) {
            legacy_min_radius = legacy_radius;
            has_legacy_min_radius = true;
        }
        if (!has_legacy_max_radius) {
            legacy_max_radius = legacy_radius;
            has_legacy_max_radius = true;
        }
    }

    if (geometry_is_circle(geometry) && (has_legacy_min_radius || has_legacy_max_radius)) {
        if (legacy_min_radius <= 0 && legacy_max_radius > 0) {
            legacy_min_radius = legacy_max_radius;
        }
        if (legacy_max_radius <= 0 && legacy_min_radius > 0) {
            legacy_max_radius = legacy_min_radius;
        }
        if (legacy_max_radius < legacy_min_radius) {
            std::swap(legacy_min_radius, legacy_max_radius);
        }

        const int migrated_min_diameter = legacy_min_radius > 0 ? legacy_min_radius * 2 : 0;
        const int migrated_max_diameter = legacy_max_radius > 0 ? legacy_max_radius * 2 : migrated_min_diameter;
        if (!has_width_min && migrated_min_diameter > 0) width_min = migrated_min_diameter;
        if (!has_width_max && migrated_max_diameter > 0) width_max = migrated_max_diameter;
        if (!has_height_min && migrated_min_diameter > 0) height_min = migrated_min_diameter;
        if (!has_height_max && migrated_max_diameter > 0) height_max = migrated_max_diameter;
    }

    sanitize_dimension_pair(width_min, width_max);
    sanitize_dimension_pair(height_min, height_max);

    int existing_value = 0;
    const bool has_numeric_min_width = entry.contains("min_width") && json_to_int(entry["min_width"], existing_value);
    if (!has_numeric_min_width || existing_value != width_min) {
        entry["min_width"] = width_min;
        changed = true;
    }
    const bool has_numeric_max_width = entry.contains("max_width") && json_to_int(entry["max_width"], existing_value);
    if (!has_numeric_max_width || existing_value != width_max) {
        entry["max_width"] = width_max;
        changed = true;
    }
    const bool has_numeric_min_height = entry.contains("min_height") && json_to_int(entry["min_height"], existing_value);
    if (!has_numeric_min_height || existing_value != height_min) {
        entry["min_height"] = height_min;
        changed = true;
    }
    const bool has_numeric_max_height = entry.contains("max_height") && json_to_int(entry["max_height"], existing_value);
    if (!has_numeric_max_height || existing_value != height_max) {
        entry["max_height"] = height_max;
        changed = true;
    }

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
    normalize_bool("is_spawn", false);
    normalize_bool("is_boss", false);
    normalize_bool("inherits_map_assets", false);

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

    if (entry.contains("curvyness")) {
        int curvyness = 0;
        (void)json_to_int(entry["curvyness"], curvyness);
        curvyness = std::max(0, curvyness);
        const bool has_numeric_curvy = json_to_int(entry["curvyness"], existing_value);
        if (!has_numeric_curvy || existing_value != curvyness) {
            entry["curvyness"] = curvyness;
            changed = true;
        }
    }

    if (!entry.contains("spawn_groups") || !entry["spawn_groups"].is_array()) {
        entry["spawn_groups"] = nlohmann::json::array();
        changed = true;
    }

    return changed;
}

bool normalize_room_config_section(nlohmann::json& section) {
    if (!section.is_object()) {
        section = nlohmann::json::object();
        return true;
    }

    bool changed = false;
    for (auto it = section.begin(); it != section.end(); ++it) {
        if (normalize_room_config_entry(it.value(), it.key())) {
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
    entry["is_spawn"] = true;
    entry["is_boss"] = false;
    entry["inherits_map_assets"] = false;
    entry["spawn_groups"] = nlohmann::json::array();
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

std::string infer_spawn_room_name(nlohmann::json& rooms_data, bool& changed) {
    if (!rooms_data.is_object()) {
        rooms_data = nlohmann::json::object();
        changed = true;
    }

    for (auto it = rooms_data.begin(); it != rooms_data.end(); ++it) {
        if (it.value().is_object()) {
            bool is_spawn = false;
            if (it.value().contains("is_spawn")) {
                if (it.value()["is_spawn"].is_boolean()) {
                    is_spawn = it.value()["is_spawn"].get<bool>();
                } else if (it.value()["is_spawn"].is_number_integer()) {
                    is_spawn = it.value()["is_spawn"].get<int>() != 0;
                }
            }
            if (is_spawn) {
                return it.key();
            }
        }
    }

    if (rooms_data.contains("spawn")) {
        if (!rooms_data["spawn"].is_object()) {
            rooms_data["spawn"] = make_default_spawn_room("spawn");
            changed = true;
        }
        return "spawn";
    }

    rooms_data["spawn"] = make_default_spawn_room("spawn");
    changed = true;
    return "spawn";
}

bool ensure_map_layers(nlohmann::json& map_manifest, const std::string& map_id) {
    nlohmann::json& rooms_data = map_manifest["rooms_data"];
    bool changed = false;
    const std::string spawn_name = infer_spawn_room_name(rooms_data, changed);

    auto layers_it = map_manifest.find("map_layers");
    const bool missing_or_empty =
        (layers_it == map_manifest.end()) ||
        !layers_it->is_array() ||
        layers_it->empty();

    if (!missing_or_empty) {
        return changed;
    }

    nlohmann::json inferred_layer = nlohmann::json::object();
    inferred_layer["level"] = 0;
    inferred_layer["max_rooms"] = 1;
    inferred_layer["rooms"] = nlohmann::json::array({
        nlohmann::json::object({
            {"name", spawn_name.empty() ? std::string("spawn") : spawn_name},
            {"max_instances", 1}
        })
    });
    map_manifest["map_layers"] = nlohmann::json::array({std::move(inferred_layer)});
    changed = true;

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
    spawn_spec["name"] = "spawn";
    spawn_spec["min_instances"] = 1;
    spawn_spec["max_instances"] = 1;
    layer["rooms"] = nlohmann::json::array({spawn_spec});
    map_info["map_layers"] = nlohmann::json::array({layer});

    map_info["map_boundary_data"] = nlohmann::json::object({
        {"inherits_map_assets", false},
        {"candidate_selectors",
         nlohmann::json::array({make_batch_spawn_group(map_name, "map_boundary", "batch_map_boundary")})}
    });
    map_info["trails_data"] = nlohmann::json::object({
        {"basic", nlohmann::json::object({
            {"name", "basic"},
            {"display_color", nlohmann::json::array({85, 242, 143, 255})},
            {"edge_smoothness", 2},
            {"geometry", "Line"},
            {"inherits_map_assets", false},
            {"is_spawn", false},
            {"is_boss", false},
            {"min_width", 400},
            {"max_width", 800},
            {"min_height", 400},
            {"max_height", 800},
            {"spawn_groups", nlohmann::json::array()}
        })}
    });

    map_info["map_layers_settings"] = nlohmann::json::object({{"min_edge_distance", 200}});

    nlohmann::json spawn_room = nlohmann::json::object();
    spawn_room["name"] = "spawn";
    spawn_room["geometry"] = "Circle";
    spawn_room["min_width"] = diameter;
    spawn_room["max_width"] = diameter;
    spawn_room["min_height"] = diameter;
    spawn_room["max_height"] = diameter;
    spawn_room["edge_smoothness"] = 2;
    spawn_room["curvyness"] = 2;
    spawn_room["is_spawn"] = true;
    spawn_room["is_boss"] = false;
    spawn_room["inherits_map_assets"] = true;
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

    map_info["rooms_data"] = nlohmann::json::object();
    map_info["rooms_data"]["spawn"] = std::move(spawn_room);
    map_info["camera_settings"] = nlohmann::json::object({
        {"smooth_motion_height", true},
        {"base_height_px", 720.0},
        {"min_visible_screen_ratio", 0.01}
    });
    map_info["map_grid_settings"] = nlohmann::json::object({{"grid_resolution", 6}});
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
                                                      const std::filesystem::path& manifest_root) {
    MapManifestNormalizationResult result;
    bool changed = false;

    if (!map_manifest.is_object()) {
        map_manifest = nlohmann::json::object();
        changed = true;
    }

    const std::array<const char*, 3> object_sections{
        "map_boundary_data",
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

    if (ensure_map_layers(map_manifest, map_id)) {
        changed = true;
    }
    if (normalize_room_config_section(map_manifest["rooms_data"])) {
        changed = true;
    }
    if (normalize_room_config_section(map_manifest["trails_data"])) {
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
                                                                       manifest_root);

    bootstrap.map_manifest = std::move(normalized.map_manifest);
    bootstrap.resolved_content_root = std::move(normalized.resolved_content_root);
    bootstrap.manifest_entry_found = manifest_entry_found;
    bootstrap.changed = normalized.changed || !manifest_entry_found;
    return bootstrap;
}

}
