#include "gameplay/world/chunk.hpp"

#if defined(ENGINE_WORLD_TESTS)

namespace world {

Chunk::Chunk(int in_i, int in_j, int r, GridBounds bounds)
    : i(in_i), j(in_j), r_chunk(r), world_bounds(bounds) {}

Chunk::~Chunk() = default;

void Chunk::releaseTileTextures() {
    tiles.clear();
}

} // namespace world

#endif // ENGINE_WORLD_TESTS

