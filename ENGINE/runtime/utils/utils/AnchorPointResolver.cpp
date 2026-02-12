#include "AnchorPointResolver.hpp"

#include <cmath>

#include "animation/animation_update.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/world_grid.hpp"

namespace anchor_points {

ResolvedAnchor resolve_anchor_point(const Asset& asset, const DisplacedAssetAnchorPoint& anchor) {
    ResolvedAnchor resolved{};
    resolved.rotation_deg = anchor.rotation_deg;

    const SDL_Point base = animation_update::detail::bottom_middle_for(asset, asset.world_point());
    const float     height_px = asset.runtime_height_px();
    const float     offset_x = anchor.px * height_px;
    const float     offset_y = anchor.py * height_px;
    const float     offset_z = anchor.pz * height_px;

    resolved.world_px.x = base.x + static_cast<int>(std::lround(offset_x));
    resolved.world_px.y = base.y + static_cast<int>(std::lround(offset_y));

    const int layer = asset.grid_point() ? asset.grid_point()->resolution_layer() : asset.grid_resolution;
    const int world_z = asset.world_z() + static_cast<int>(std::lround(offset_z));

    if (auto* assets_owner = asset.get_assets()) {
        world::WorldGrid& grid = assets_owner->world_grid();
        world::GridKey key{ resolved.world_px.x, resolved.world_px.y, world_z, layer };
        resolved.grid_point = grid.find_grid_point_strict(key);
        if (!resolved.grid_point) {
            resolved.grid_point = &grid.find_or_create_grid_point(key);
        }
    }

    return resolved;
}

}
