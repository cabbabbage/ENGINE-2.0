#include "gameplay/world/chunk_manager.hpp"

#include <algorithm>

#include "assets/Asset.hpp"

namespace world {

int floor_div(int value, int step);

std::uint64_t ChunkManager::key(int i, int j) {
    const auto hi = static_cast<std::uint32_t>(i);
    const auto lo = static_cast<std::uint32_t>(j);
    return (static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo);
}

GridBounds ChunkManager::bounds_for(int i, int j, int r_chunk, const GridPoint& origin) {
    const int step = 1 << r_chunk;
    const int x = origin.world_x() + i * step;
    const int z = origin.world_z() + j * step;
    return GridBounds::from_xywh(x, z, step, step, 0, origin.resolution_layer());
}

Chunk& ChunkManager::ensure(int i, int j, int r_chunk, const GridPoint& origin) {
    const auto k = key(i, j);
    if (auto it = lookup_.find(k); it != lookup_.end()) {
        Chunk* existing = it->second;
        return *existing;
    }
    GridBounds rect = bounds_for(i, j, r_chunk, origin);
    auto chunk = std::make_unique<Chunk>(i, j, r_chunk, rect);
    Chunk* raw = chunk.get();
    storage_.push_back(std::move(chunk));
    lookup_.emplace(k, raw);
    return *raw;
}

Chunk* ChunkManager::find(int i, int j) const {
    const auto k = key(i, j);
    const auto it = lookup_.find(k);
    return it == lookup_.end() ? nullptr : it->second;
}

Chunk* ChunkManager::from_world(const GridPoint& world_px, int r_chunk, const GridPoint& origin) const {
    const int step = 1 << r_chunk;
    const int i = floor_div(world_px.world_x() - origin.world_x(), step);
    const int k = floor_div(world_px.world_z() - origin.world_z(), step);
    return find(i, k);
}

int floor_div(int value, int step) {
    if (step == 0) {
        return 0;
    }
    int quotient = value / step;
    int remainder = value % step;
    if (remainder != 0 && ((remainder > 0) != (step > 0))) {
        --quotient;
    }
    return quotient;
}

}

