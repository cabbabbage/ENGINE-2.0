#include "map_grid_settings.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <nlohmann/json.hpp>

#include "utils/grid.hpp"

namespace {
constexpr int kMinResolution = 0;
constexpr int kMaxResolution = vibble::grid::kMaxResolution;

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

int resolution_from_spacing(int spacing) {
    const int clamped = std::max(1, spacing);
    const double log_value = std::log(static_cast<double>(clamped)) / std::log(3.0);
    return static_cast<int>(std::lround(log_value));
}

int resolution_from_chunk_size(int chunk_size) {
    const int clamped = std::max(1, chunk_size);
    const double log_value = std::log2(static_cast<double>(clamped));
    return static_cast<int>(std::lround(log_value));
}
}

MapGridSettings MapGridSettings::defaults() {
    return MapGridSettings{0};
}

MapGridSettings MapGridSettings::from_json(const nlohmann::json* obj) {
    MapGridSettings settings = defaults();
    if (!obj || !obj->is_object()) {
        return settings;
    }
    bool resolved = false;
    try {
        if (obj->contains("grid_resolution") && (*obj)["grid_resolution"].is_number_integer()) {
            settings.grid_resolution = (*obj)["grid_resolution"].get<int>();
            resolved = true;
        }
    } catch (...) {
        settings.grid_resolution = defaults().grid_resolution;
    }

    try {
        if (!resolved && obj->contains("resolution") && (*obj)["resolution"].is_number_integer()) {
            settings.grid_resolution = (*obj)["resolution"].get<int>();
            resolved = true;
        }
    } catch (...) {
        if (!resolved) {
            settings.grid_resolution = defaults().grid_resolution;
        }
    }

    try {
        if (!resolved && obj->contains("spacing") && (*obj)["spacing"].is_number_integer()) {
            const int spacing = std::max(1, (*obj)["spacing"].get<int>());
            settings.grid_resolution = resolution_from_spacing(spacing);
            resolved = true;
        }
    } catch (...) {
        if (!resolved) {
            settings.grid_resolution = defaults().grid_resolution;
        }
    }

    try {
        if (!resolved && obj->contains("r_chunk") && (*obj)["r_chunk"].is_number_integer()) {
            settings.grid_resolution = (*obj)["r_chunk"].get<int>();
            resolved = true;
        } else if (!resolved && obj->contains("chunk_resolution") && (*obj)["chunk_resolution"].is_number_integer()) {
            settings.grid_resolution = (*obj)["chunk_resolution"].get<int>();
            resolved = true;
        } else if (!resolved) {
            const char* size_keys[] = {"chunk_size", "chunk_size_px"};
            for (const char* key : size_keys) {
                if (obj->contains(key) && (*obj)[key].is_number_integer()) {
                    const int size_px = std::max(1, (*obj)[key].get<int>());
                    settings.grid_resolution = resolution_from_chunk_size(size_px);
                    resolved = true;
                    break;
                }
            }
        }
    } catch (...) {
        if (!resolved) {
            settings.grid_resolution = defaults().grid_resolution;
        }
    }

    settings.clamp();
    return settings;
}

void MapGridSettings::clamp() {
    grid_resolution = std::clamp(grid_resolution, kMinResolution, kMaxResolution);
}

void MapGridSettings::apply_to_json(nlohmann::json& obj) const {
    if (!obj.is_object()) {
        obj = nlohmann::json::object();
    }
    obj["grid_resolution"] = grid_resolution;
    obj.erase("resolution");
    obj.erase("spacing");
    obj.erase("jitter");
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

int MapGridSettings::chunk_size() const {
    const int clamped = std::clamp(grid_resolution, kMinResolution, kMaxResolution);
    return 1 << clamped;
}

int MapGridSettings::tile_spacing() const {
    return spacing();
}
