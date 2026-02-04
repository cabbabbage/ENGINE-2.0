#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "gameplay/world/tiling/grid_tile.hpp"
#include "gameplay/world/grid_point.hpp"

class Assets;
class Asset;
class WarpedScreenGrid;
namespace world {
class Grid;
}
namespace world {

struct Chunk {
    int      i = 0;
    int      j = 0;
    int      r_chunk = 0;
    GridBounds world_bounds = GridBounds::from_xywh(0, 0, 0, 0);

    std::vector<Asset*> assets;
    std::uint64_t       occlusion_revision = 0;

    std::vector<GridTile> tiles;

    Chunk() = default;
    Chunk(int in_i, int in_j, int r, GridBounds bounds);
    ~Chunk();
    void releaseTileTextures();

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) noexcept = default;
    Chunk& operator=(Chunk&&) noexcept = default;
};

}
