#include "map_grid_settings.hpp"

#include <algorithm>
#include <limits>

#include <nlohmann/json.hpp>

#include "utils/grid.hpp"

namespace {
constexpr int kMinResolution = 0;
constexpr int kMaxResolution = vibble::grid::kMaxResolution;
constexpr int kMinJitter = 0;
constexpr int kMaxJitter = 500;

int power_of_three(int exponent) {
    if (exponent <= 0) {
        return 1;
    }
    int result = 1;
    for (int i = 0; i < exponent; ++i) {
        if (result > std::numeric_limits<int>::max() / 3) {
            return std::numeric_limits<int>::max();
        }
        result *= 3;
    }
    return result;
}
}

MapGridSettings MapGridSettings::defaults() {
    return MapGridSettings{0, 0};
}

MapGridSettings MapGridSettings::from_json(const nlohmann::json* obj) {
    MapGridSettings settings = defaults();
    if (!obj || !obj->is_object()) {
        return settings;
    }

    try {
        if (obj->contains("grid_resolution") && (*obj)["grid_resolution"].is_number_integer()) {
            settings.grid_resolution = (*obj)["grid_resolution"].get<int>();
        }
        if (obj->contains("position_jitter_px") && (*obj)["position_jitter_px"].is_number_integer()) {
            settings.position_jitter_px = (*obj)["position_jitter_px"].get<int>();
        }
    } catch (...) {
        settings.grid_resolution = defaults().grid_resolution;
        settings.position_jitter_px = defaults().position_jitter_px;
    }

    settings.clamp();
    return settings;
}

void MapGridSettings::clamp() {
    grid_resolution = std::clamp(grid_resolution, kMinResolution, kMaxResolution);
    position_jitter_px = std::clamp(position_jitter_px, kMinJitter, kMaxJitter);
}

void MapGridSettings::apply_to_json(nlohmann::json& obj) const {
    if (!obj.is_object()) {
        obj = nlohmann::json::object();
    }
    obj["grid_resolution"] = grid_resolution;
    obj["position_jitter_px"] = position_jitter_px;
    obj.erase("resolution");
    obj.erase("r_chunk");
    obj.erase("chunk_resolution");
    obj.erase("chunk_size");
    obj.erase("chunk_size_px");
    obj.erase("tile_resolution");
}

void ensure_map_grid_settings(nlohmann::json& map_info) {
    if (!map_info.is_object()) {
        map_info = nlohmann::json::object();
    }
    nlohmann::json& section = map_info["map_grid_settings"];
    if (!section.is_object()) {
        section = nlohmann::json::object();
    }
    MapGridSettings settings = MapGridSettings::from_json(&section);
    settings.apply_to_json(section);
}

int MapGridSettings::spacing() const {
    const int clamped = std::clamp(grid_resolution, kMinResolution, kMaxResolution);
    return power_of_three(clamped);
}
