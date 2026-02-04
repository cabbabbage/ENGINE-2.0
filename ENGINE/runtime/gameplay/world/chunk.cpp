#include "gameplay/world/chunk.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/render.hpp"
#include "gameplay/world/world_grid.hpp"

namespace world {

Chunk::Chunk(int in_i, int in_j, int r, GridBounds bounds)
    : i(in_i)
    , j(in_j)
    , r_chunk(r)
    , world_bounds(bounds) {}

Chunk::~Chunk() {
    releaseTileTextures();
}
void Chunk::releaseTileTextures() {
    for (auto& t : tiles) {
        if (t.texture) {
            SDL_DestroyTexture(t.texture);
            t.texture = nullptr;
        }
    }
    tiles.clear();
}
}
