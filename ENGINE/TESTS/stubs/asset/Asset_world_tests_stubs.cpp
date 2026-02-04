#include "asset/Asset.hpp"

Asset::~Asset() = default;

void Asset::clear_grid_id() {
    grid_id_ = 0;
}
