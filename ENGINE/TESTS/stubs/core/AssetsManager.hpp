#pragma once

#include "gameplay/world/world_grid.hpp"

class Assets {
public:
    Assets() = default;

    world::WorldGrid& world_grid() { return grid_; }
    const world::WorldGrid& world_grid() const { return grid_; }

private:
    world::WorldGrid grid_{};
};

