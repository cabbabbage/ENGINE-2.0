#pragma once

#include <nlohmann/json_fwd.hpp>

class Area;

struct MapGridSettings {
    int grid_resolution = 0; // Single resolution value for grid spacing and tile/chunk sizing.

    static MapGridSettings defaults();
    static MapGridSettings from_json(const nlohmann::json* obj);

    void clamp();
    void apply_to_json(nlohmann::json& obj) const;

    int spacing() const;     // 3^grid_resolution spacing for GridPoint placement.
    int chunk_size() const;  // Tile chunk size (power-of-two, tile-only).
    int tile_spacing() const; // Alias for spacing when tiling assets.
};

void ensure_map_grid_settings(nlohmann::json& map_info);
