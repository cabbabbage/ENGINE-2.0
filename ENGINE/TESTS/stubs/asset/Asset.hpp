#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <vector>

#include "gameplay/world/grid_point.hpp"

namespace world { class WorldGrid; }
class AssetInfo;

// Minimal stub for tests targeting WorldGrid only.
// Provides just enough API used by world_grid.cpp
class Asset {
 public:
    ~Asset();

    SDL_Point pos{0, 0};
    int grid_resolution = 0;
    int pos_z = 0;
    AssetInfo* info = nullptr;

    int world_x() const { return pos_ ? pos_->world_x() : pos.x; }
    int world_y() const { return pos_ ? pos_->world_y() : pos.y; }
    int world_z() const { return pos_ ? pos_->world_z() : pos_z; }
    SDL_Point world_point() const { return SDL_Point{world_x(), world_y()}; }

    void set_grid_id(std::uint64_t id) { grid_id_ = id; }
    std::uint64_t grid_id() const { return grid_id_; }
    void clear_grid_id();

private:
    friend class world::WorldGrid;
    world::GridPoint* pos_ = nullptr;
    std::uint64_t grid_id_ = 0;
};
