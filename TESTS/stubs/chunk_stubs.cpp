#include <SDL.h>
#include <utility>

#include "asset/Asset.hpp"
#include "world/chunk.hpp"

namespace world {

Chunk::Chunk(int in_i, int in_j, int r, GridBounds bounds)
    : i(in_i), j(in_j), r_chunk(r), world_bounds(std::move(bounds)) {}

Chunk::~Chunk() = default;

} // namespace world
