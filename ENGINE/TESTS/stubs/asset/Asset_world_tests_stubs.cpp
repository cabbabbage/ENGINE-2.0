#include "../../../runtime/core/asset_list.hpp"
#include "../../../runtime/animation/animation_runtime.hpp" // ensure AnimRuntime is complete before defaulting the destructor
#include "../../../runtime/assets/Asset.hpp"

Asset::~Asset() = default;

void Asset::clear_grid_id() {
    grid_id_ = 0;
}
