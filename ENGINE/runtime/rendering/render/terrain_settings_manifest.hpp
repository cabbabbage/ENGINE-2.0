#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "rendering/render/terrain_settings.hpp"

namespace terrain_manifest {

// Parse TerrainSettings from a map manifest JSON object.
// Falls back to the current sanitized TerrainSettings::readonly() values when
// the manifest is missing or malformed.
inline TerrainSettings read_settings(const nlohmann::json* map_info) {
    TerrainSettings settings = TerrainSettings::sanitized(TerrainSettings::readonly());
    if (!map_info || !map_info->is_object()) {
        return settings;
    }

    auto read_float = [](const nlohmann::json& obj, const char* key, float fallback) -> float {
        auto it = obj.find(key);
        if (it == obj.end()) return fallback;
        if (it->is_number_float())   return static_cast<float>(it->get<double>());
        if (it->is_number_integer()) return static_cast<float>(it->get<std::int64_t>());
        if (it->is_number_unsigned()) return static_cast<float>(it->get<std::uint64_t>());
        return fallback;
    };

    auto read_bool = [](const nlohmann::json& obj, const char* key, bool fallback) -> bool {
        auto it = obj.find(key);
        return (it != obj.end() && it->is_boolean()) ? it->get<bool>() : fallback;
    };

    auto read_vec2 = [](const nlohmann::json& obj, const char* key, SDL_FPoint fallback) -> SDL_FPoint {
        auto it = obj.find(key);
        if (it == obj.end()) return fallback;
        SDL_FPoint out = fallback;
        if (it->is_array() && it->size() >= 2) {
            if ((*it)[0].is_number()) out.x = static_cast<float>((*it)[0].get<double>());
            if ((*it)[1].is_number()) out.y = static_cast<float>((*it)[1].get<double>());
            return out;
        }
        if (it->is_object()) {
            auto x_it = it->find("x");
            auto y_it = it->find("y");
            if (x_it != it->end() && x_it->is_number()) out.x = static_cast<float>(x_it->get<double>());
            if (y_it != it->end() && y_it->is_number()) out.y = static_cast<float>(y_it->get<double>());
        }
        return out;
    };

    try {
        auto ts_it = map_info->find("terrain_settings");
        if (ts_it == map_info->end() || !ts_it->is_object()) {
            return settings;
        }

        const auto& ts = *ts_it;
        settings.enabled = read_bool(ts, "enabled", settings.enabled);
        settings.max_elevation_world = read_float(ts, "max_elevation_world", settings.max_elevation_world);
        settings.edge_falloff_distance_world = read_float(ts, "edge_falloff_distance_world", settings.edge_falloff_distance_world);
        settings.smoothness = read_float(ts, "smoothness", settings.smoothness);
        settings.noise_variation = read_float(ts, "noise_variation", settings.noise_variation);
        settings.roughness = read_float(ts, "roughness", settings.roughness);
        settings.blend_strength = read_float(ts, "blend_strength", settings.blend_strength);
        settings.resolution_density_scale = read_float(ts, "resolution_density_scale", settings.resolution_density_scale);

        auto light_it = ts.find("light");
        if (light_it != ts.end() && light_it->is_object()) {
            const auto& lj = *light_it;
            auto seed_it = lj.find("base_seed");
            if (seed_it != lj.end() && seed_it->is_number()) {
                std::uint64_t raw_seed = 0;
                if (seed_it->is_number_unsigned()) {
                    raw_seed = seed_it->get<std::uint64_t>();
                } else if (seed_it->is_number_integer()) {
                    raw_seed = static_cast<std::uint64_t>(seed_it->get<std::int64_t>());
                } else if (seed_it->is_number_float()) {
                    raw_seed = static_cast<std::uint64_t>(seed_it->get<double>());
                }
                settings.light.base_seed = static_cast<std::uint32_t>(raw_seed & 0xffffffffu);
            }
            settings.light.lock_seed_to_world = read_bool(lj, "lock_seed_to_world", settings.light.lock_seed_to_world);
            settings.light.direction_world = read_vec2(lj, "direction_world", settings.light.direction_world);
            settings.light.position_world = read_vec2(lj, "position_world", settings.light.position_world);
            settings.light.light_strength = read_float(lj, "light_strength", settings.light.light_strength);
            settings.light.contrast = read_float(lj, "contrast", settings.light.contrast);
        }
    } catch (...) {
        // Swallow malformed manifest data and fall back to sanitized defaults.
    }

    return TerrainSettings::sanitized(settings);
}

// Persist TerrainSettings back into a map manifest JSON object. Existing
// boolean "randomize_seed"/"randomize_session_seed" flags (if present) are
// preserved even though they are not part of TerrainSettings.
inline void write_settings(nlohmann::json& map_info, const TerrainSettings& incoming) {
    TerrainSettings settings = TerrainSettings::sanitized(incoming);

    if (!map_info.is_object()) {
        map_info = nlohmann::json::object();
    }

    bool randomize_seed = false;
    bool randomize_session_seed = false;
    bool has_randomize_seed = false;
    bool has_randomize_session_seed = false;
    if (auto it = map_info.find("terrain_settings"); it != map_info.end() && it->is_object()) {
        const auto& existing = *it;
        if (auto r_it = existing.find("randomize_seed"); r_it != existing.end() && r_it->is_boolean()) {
            randomize_seed = r_it->get<bool>();
            has_randomize_seed = true;
        }
        if (auto r2_it = existing.find("randomize_session_seed"); r2_it != existing.end() && r2_it->is_boolean()) {
            randomize_session_seed = r2_it->get<bool>();
            has_randomize_session_seed = true;
        }
    }

    nlohmann::json light = nlohmann::json::object();
    light["base_seed"] = settings.light.base_seed;
    light["lock_seed_to_world"] = settings.light.lock_seed_to_world;
    light["direction_world"] = { settings.light.direction_world.x, settings.light.direction_world.y };
    light["position_world"] = { settings.light.position_world.x, settings.light.position_world.y };
    light["light_strength"] = settings.light.light_strength;
    light["contrast"] = settings.light.contrast;

    nlohmann::json ts = nlohmann::json::object({
        {"enabled", settings.enabled},
        {"max_elevation_world", settings.max_elevation_world},
        {"edge_falloff_distance_world", settings.edge_falloff_distance_world},
        {"smoothness", settings.smoothness},
        {"noise_variation", settings.noise_variation},
        {"roughness", settings.roughness},
        {"blend_strength", settings.blend_strength},
        {"resolution_density_scale", settings.resolution_density_scale},
        {"light", std::move(light)}
    });

    if (has_randomize_seed) {
        ts["randomize_seed"] = randomize_seed;
    }
    if (has_randomize_session_seed) {
        ts["randomize_session_seed"] = randomize_session_seed;
    }

    map_info["terrain_settings"] = std::move(ts);
}

}  // namespace terrain_manifest

