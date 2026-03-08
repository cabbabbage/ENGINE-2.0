#include "FramePointResolver.hpp"
#include "gameplay/world/world_grid.hpp"
#include <algorithm>
#include <cmath>

#include "animation/animation_update.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/grid_point.hpp"
#include "utils/AnchorPointResolver.hpp"

namespace devmode::frame_editors {

SDL_Point FramePointResolver::anchor_world() const {
    if (!asset_) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*asset_, asset_->world_point());
}

float FramePointResolver::parent_height_px() const {
    if (!asset_) {
        return 0.0f;
    }
    return anchor_points::anchor_height_px(*asset_);
}

float FramePointResolver::base_world_height() const {
    if (!asset_) {
        return 0.0f;
    }
    return static_cast<float>(asset_->world_y());
}

float FramePointResolver::base_world_depth() const {
    if (!asset_) {
        return 0.0f;
    }
    return static_cast<float>(asset_->world_z());
}

float FramePointResolver::to_percent_height(float world_height) const {
    const float height = parent_height_px();
    if (height <= 0.0f) {
        return 0.0f;
    }
    const float base_height = base_world_height();
    return (world_height - base_height) / height;
}

float FramePointResolver::to_world_height(float height_percent) const {
    if (!asset_) {
        return 0.0f;
    }
    const float height = parent_height_px();
    const float base_height = base_world_height();
    return base_height + (height_percent * height);
}

float FramePointResolver::to_percent_depth(float world_depth) const {
    const float height = parent_height_px();
    if (height <= 0.0f) {
        return 0.0f;
    }
    const float base_depth = base_world_depth();
    return (world_depth - base_depth) / height;
}

float FramePointResolver::to_world_depth(float depth_percent) const {
    if (!asset_) {
        return 0.0f;
    }
    const float height = parent_height_px();
    const float base_depth = base_world_depth();
    return base_depth + (depth_percent * height);
}

float FramePointResolver::to_percent_xy(float world_coord) const {
    const float height = parent_height_px();
    if (height <= 0.0f) {
        return 0.0f;
    }
    return world_coord / height;
}

float FramePointResolver::to_world_xy(float coord_percent) const {
    const float height = parent_height_px();
    if (height <= 0.0f) {
        return 0.0f;
    }
    return coord_percent * height;
}

FramePointResolver::Displacement_percent_vals FramePointResolver::to_percent_displacement(int x, int y, int z, const Asset* source_asset) const {
    FramePointResolver::Displacement_percent_vals vals{};
    if (!source_asset) {
        return vals;
    }

    const float source_height = anchor_points::anchor_height_px(*source_asset);

    if (source_height > 0.0f) {
        vals.dx_percent = static_cast<float>(x) / source_height;
        vals.dy_percent = static_cast<float>(y) / source_height;
        vals.dz_percent = static_cast<float>(z) / source_height;
    }
    return vals;
}

world::GridPoint* FramePointResolver::to_grid_point_displacement(FramePointResolver::Displacement_percent_vals vals, const Asset* source_asset) const {
    if (!source_asset) {
        return nullptr;
    }
    world::GridPoint* source_gp = source_asset->grid_point();
    if (!source_gp) return nullptr;

    Assets* assets = source_asset->get_assets();
    if (!assets) return nullptr;

    const float source_height = anchor_points::anchor_height_px(*source_asset);
    if (!(source_height > 0.0f)) {
        return nullptr;
    }

    const int displaced_x = source_gp->world_x() + static_cast<int>(std::lround(vals.dx_percent * source_height));
    const int displaced_y = source_gp->world_y() + static_cast<int>(std::lround(vals.dy_percent * source_height));
    const int displaced_z = source_gp->world_z() + static_cast<int>(std::lround(vals.dz_percent * source_height));

    world::GridKey key{displaced_x, displaced_y, displaced_z, source_gp->resolution_layer()};
    return assets->world_grid().find_grid_point_strict(key);
}

}  // namespace devmode::frame_editors
