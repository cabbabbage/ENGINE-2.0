#pragma once

#include <nlohmann/json_fwd.hpp>

struct MapGridSettings {
    int grid_resolution = 0;      // Resolution for grid spacing and chunk hierarchy behavior.
    int position_jitter_px = 0;   // Random x/z offset applied to map-wide asset spawns (0 = no jitter).
    int tile_step_px = 0;         // Explicit tiled asset step in world px (0 = auto from asset dimensions).

    static MapGridSettings defaults();
    static MapGridSettings from_json(const nlohmann::json* obj);

    void clamp();
    void apply_to_json(nlohmann::json& obj) const;

    int spacing() const; // 3^grid_resolution spacing for GridPoint placement.
};

int clamp_tiled_asset_step_px(int step_px);
int resolve_tiled_asset_step_px(const MapGridSettings& settings, int fallback_step_px);

void ensure_map_grid_settings(nlohmann::json& map_info);
