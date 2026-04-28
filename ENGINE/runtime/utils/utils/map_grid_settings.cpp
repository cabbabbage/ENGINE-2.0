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
constexpr int kMinTileStepPx = 0;
constexpr int kMaxTileStepPx = 8192;
constexpr int kMinResolvedTileStepPx = 1;
constexpr int kMaxResolvedTileStepPx = 8192;

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
    return MapGridSettings{8, 0, 0};
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
        if (obj->contains("tile_step_px") && (*obj)["tile_step_px"].is_number_integer()) {
            settings.tile_step_px = (*obj)["tile_step_px"].get<int>();
        } else if (obj->contains("tile_size_px") && (*obj)["tile_size_px"].is_number_integer()) {
            settings.tile_step_px = (*obj)["tile_size_px"].get<int>();
        }
    } catch (...) {
        settings.grid_resolution = defaults().grid_resolution;
        settings.position_jitter_px = defaults().position_jitter_px;
        settings.tile_step_px = defaults().tile_step_px;
    }

    settings.clamp();
    return settings;
}

void MapGridSettings::clamp() {
    grid_resolution = std::clamp(grid_resolution, kMinResolution, kMaxResolution);
    position_jitter_px = std::clamp(position_jitter_px, kMinJitter, kMaxJitter);
    tile_step_px = std::clamp(tile_step_px, kMinTileStepPx, kMaxTileStepPx);
}

void MapGridSettings::apply_to_json(nlohmann::json& obj) const {
    if (!obj.is_object()) {
        obj = nlohmann::json::object();
    }
    obj["grid_resolution"] = grid_resolution;
    obj["position_jitter_px"] = position_jitter_px;
    obj["tile_step_px"] = tile_step_px;
    obj.erase("resolution");
    obj.erase("r_chunk");
    obj.erase("chunk_resolution");
    obj.erase("chunk_size");
    obj.erase("chunk_size_px");
    obj.erase("tile_resolution");
    obj.erase("tile_size_px");
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

int clamp_tiled_asset_step_px(int step_px) {
    return std::clamp(step_px, kMinResolvedTileStepPx, kMaxResolvedTileStepPx);
}

int resolve_tiled_asset_step_px(const MapGridSettings& settings, int fallback_step_px) {
    if (settings.tile_step_px > 0) {
        return clamp_tiled_asset_step_px(settings.tile_step_px);
    }

    int resolved = fallback_step_px;
    if (resolved <= 0) {
        resolved = settings.spacing();
    }
    return clamp_tiled_asset_step_px(std::max(1, resolved));
}
