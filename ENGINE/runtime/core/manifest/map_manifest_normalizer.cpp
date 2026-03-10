#include "core/manifest/map_manifest_normalizer.hpp"

#include <array>

#include "utils/map_grid_settings.hpp"

namespace manifest {
namespace {

constexpr int kDefaultSpawnRadius = 1500;

bool ensure_object_section(nlohmann::json& root, const char* key) {
    auto it = root.find(key);
    if (it == root.end() || !it->is_object()) {
        root[key] = nlohmann::json::object();
        return true;
    }
    return false;
}

bool ensure_fog_defaults(nlohmann::json& root) {
    bool changed = false;
    if (ensure_object_section(root, "fog_settings")) {
        changed = true;
    }

    nlohmann::json& fog = root["fog_settings"];
    auto jitter = fog.find("max_random_jitter");
    if (jitter == fog.end() || !jitter->is_number()) {
        fog["max_random_jitter"] = 0;
        changed = true;
    }
    return changed;
}

nlohmann::json make_default_spawn_room(const std::string& spawn_name) {
    const int diameter = kDefaultSpawnRadius * 2;
    nlohmann::json entry = nlohmann::json::object();
    entry["name"] = spawn_name;
    entry["geometry"] = "Circle";
    entry["radius"] = kDefaultSpawnRadius;
    entry["min_radius"] = kDefaultSpawnRadius;
    entry["max_radius"] = kDefaultSpawnRadius;
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

std::string infer_spawn_room_name(nlohmann::json& rooms_data, bool& changed) {
    if (!rooms_data.is_object()) {
        rooms_data = nlohmann::json::object();
        changed = true;
    }

    for (auto it = rooms_data.begin(); it != rooms_data.end(); ++it) {
        if (it.value().is_object() && it.value().value("is_spawn", false)) {
            return it.key();
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

MapManifestNormalizationResult normalize_map_manifest(nlohmann::json map_manifest,
                                                      const std::string& map_id,
                                                      const std::filesystem::path& manifest_root) {
    MapManifestNormalizationResult result;
    bool changed = false;

    if (!map_manifest.is_object()) {
        map_manifest = nlohmann::json::object();
        changed = true;
    }

    const std::array<const char*, 4> object_sections{
        "map_assets_data",
        "map_boundary_data",
        "rooms_data",
        "trails_data"
    };
    for (const char* key : object_sections) {
        if (ensure_object_section(map_manifest, key)) {
            changed = true;
        }
    }

    if (ensure_fog_defaults(map_manifest)) {
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

}
