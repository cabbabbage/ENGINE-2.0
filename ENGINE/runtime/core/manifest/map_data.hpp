#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace manifest {

struct MapData {
    std::string map_id;

    nlohmann::json rooms_data = nlohmann::json::object();
    nlohmann::json trails_data = nlohmann::json::object();
    nlohmann::json map_layers = nlohmann::json::array();
    nlohmann::json map_layers_settings = nlohmann::json::object();
    nlohmann::json map_boundary_data = nlohmann::json::object();
    nlohmann::json fog_settings = nlohmann::json::object();
    nlohmann::json terrain_settings = nlohmann::json::object();
    nlohmann::json map_assets_data = nlohmann::json::object();
    nlohmann::json dev_map_settings = nlohmann::json::object();

    nlohmann::json extras = nlohmann::json::object();

    static MapData from_manifest_entry(const std::string& map_id, const nlohmann::json& json) {
        MapData data;
        data.map_id = map_id;

        const nlohmann::json entry = json.is_object() ? json : nlohmann::json::object();

        auto capture_known = [&](std::string_view key, nlohmann::json& out, const nlohmann::json& fallback) {
            auto it = entry.find(std::string(key));
            out = (it != entry.end()) ? *it : fallback;
        };

        capture_known("rooms_data", data.rooms_data, nlohmann::json::object());
        capture_known("trails_data", data.trails_data, nlohmann::json::object());
        capture_known("map_layers", data.map_layers, nlohmann::json::array());
        capture_known("map_layers_settings", data.map_layers_settings, nlohmann::json::object());
        capture_known("map_boundary_data", data.map_boundary_data, nlohmann::json::object());
        capture_known("fog_settings", data.fog_settings, nlohmann::json::object());
        capture_known("terrain_settings", data.terrain_settings, nlohmann::json::object());
        capture_known("map_assets_data", data.map_assets_data, nlohmann::json::object());
        capture_known("dev_map_settings", data.dev_map_settings, nlohmann::json::object());

        data.extras = nlohmann::json::object();
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            const std::string& key = it.key();
            if (key == "rooms_data" ||
                key == "trails_data" ||
                key == "map_layers" ||
                key == "map_layers_settings" ||
                key == "map_boundary_data" ||
                key == "fog_settings" ||
                key == "terrain_settings" ||
                key == "map_assets_data" ||
                key == "dev_map_settings") {
                continue;
            }
            data.extras[key] = *it;
        }

        return data;
    }

    nlohmann::json to_manifest_entry() const {
        nlohmann::json out = extras.is_object() ? extras : nlohmann::json::object();

        out["rooms_data"] = rooms_data;
        out["trails_data"] = trails_data;
        out["map_layers"] = map_layers;
        out["map_layers_settings"] = map_layers_settings;
        out["map_boundary_data"] = map_boundary_data;
        out["fog_settings"] = fog_settings;
        out["terrain_settings"] = terrain_settings;
        out["map_assets_data"] = map_assets_data;
        out["dev_map_settings"] = dev_map_settings;

        return out;
    }
};

} // namespace manifest

