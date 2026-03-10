#pragma once

#include <nlohmann/json_fwd.hpp>

struct MapGridSettings {
    int grid_resolution = 0; // Single resolution value for grid spacing and tile/chunk sizing.

    static MapGridSettings defaults();
    static MapGridSettings from_json(const nlohmann::json* obj);

    void clamp();
    void apply_to_json(nlohmann::json& obj) const;

    int spacing() const; // 3^grid_resolution spacing for GridPoint placement.
};

void ensure_map_grid_settings(nlohmann::json& map_info);
