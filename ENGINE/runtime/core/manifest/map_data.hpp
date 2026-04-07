#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace manifest {

inline constexpr int kMapSchemaVersion = 1;

inline std::string make_map_schema_error(const std::string& map_id, const std::string& message) {
    return "map manifest entry '" + map_id + "' " + message;
}

struct MapData {
    std::string map_id;
    int schema_version = kMapSchemaVersion;

    nlohmann::json rooms_data = nlohmann::json::object();
    nlohmann::json trails_data = nlohmann::json::object();
    nlohmann::json map_layers = nlohmann::json::array();
    nlohmann::json map_layers_settings = nlohmann::json::object();
    nlohmann::json map_boundary_data = nlohmann::json::object();
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

        auto version_it = entry.find("schema_version");
        if (version_it == entry.end() || !version_it->is_number_integer()) {
            throw std::runtime_error(make_map_schema_error(map_id, "missing \"schema_version\"."));
        }
        const int version_value = version_it->get<int>();
        if (version_value != kMapSchemaVersion) {
            throw std::runtime_error(make_map_schema_error(map_id,
                "has schema_version " + std::to_string(version_value) +
                " but expected " + std::to_string(kMapSchemaVersion) + "."));
        }
        data.schema_version = version_value;

        capture_known("rooms_data", data.rooms_data, nlohmann::json::object());
        capture_known("trails_data", data.trails_data, nlohmann::json::object());
        capture_known("map_layers", data.map_layers, nlohmann::json::array());
        capture_known("map_layers_settings", data.map_layers_settings, nlohmann::json::object());
        capture_known("map_boundary_data", data.map_boundary_data, nlohmann::json::object());
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
                key == "map_assets_data" ||
                key == "dev_map_settings" ||
                key == "schema_version") {
                continue;
            }
            data.extras[key] = *it;
        }

        return data;
    }

    nlohmann::json to_manifest_entry() const {
        nlohmann::json out = extras.is_object() ? extras : nlohmann::json::object();

        out["schema_version"] = schema_version;

        out["rooms_data"] = rooms_data;
        out["trails_data"] = trails_data;
        out["map_layers"] = map_layers;
        out["map_layers_settings"] = map_layers_settings;
        out["map_boundary_data"] = map_boundary_data;
        out["map_assets_data"] = map_assets_data;
        out["dev_map_settings"] = dev_map_settings;

        return out;
    }
};

} // namespace manifest

