#pragma once

#include <random>
#include <SDL.h>
#include <nlohmann/json_fwd.hpp>

class Area;

struct MapGridSettings {
    int resolution = 0; // 3^resolution spacing for Map Grid identity.
    int jitter = 0;
    int r_chunk = 0;    // Tile-only chunk exponent (power-of-two), legacy for tiles.
    int tile_resolution = -1; // Optional override for tile spacing (3^value). -1 uses resolution.

    static MapGridSettings defaults();
    static MapGridSettings from_json(const nlohmann::json* obj);

    void clamp();
    void apply_to_json(nlohmann::json& obj) const;

    int spacing() const;     // 3^resolution spacing for GridPoint placement.
    int chunk_size() const;  // Tile chunk size (power-of-two, tile-only).
    int tile_resolution_value() const;
    int tile_spacing() const;
};

void ensure_map_grid_settings(nlohmann::json& map_info);
SDL_Point apply_map_grid_jitter(const MapGridSettings& settings, SDL_Point base, std::mt19937& rng, const Area& area);
