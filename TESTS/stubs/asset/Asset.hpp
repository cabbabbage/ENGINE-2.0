#pragma once

#include <SDL.h>
#include <cstdint>
#include <vector>

struct AssetInfo {
    struct LightSource {
        int intensity = 0;
        bool active = false;
    };
    std::vector<LightSource> light_sources;
    bool moving_asset = false;
};

// Minimal stub for tests targeting WorldGrid only.
// Provides just enough API used by world_grid.cpp
class Asset {
public:
    SDL_Point pos{0, 0};
    AssetInfo* info = nullptr;

    void set_grid_id(std::uint64_t id) { grid_id_ = id; }
    std::uint64_t grid_id() const { return grid_id_; }
    void clear_grid_id() { grid_id_ = 0; }

private:
    std::uint64_t grid_id_ = 0;
};
