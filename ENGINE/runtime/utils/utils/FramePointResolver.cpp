#include "FramePointResolver.hpp"
#include "gameplay/world/world_grid.hpp"
#include <algorithm>
#include <cmath>

#include "animation/animation_update.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/grid_point.hpp"

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

    int height_px = asset_->height();
    if (height_px <= 0) {
        if (SDL_Texture* tex = asset_->get_current_frame()) {
            int w = 0;
            if (SDL_QueryTexture(tex, nullptr, nullptr, &w, &height_px) != 0) {
                height_px = 0;
            }
        }
    }
    return static_cast<float>(height_px);
}

float FramePointResolver::base_world_z() const {
    if (!asset_) {
        return 0.0f;
    }
    return static_cast<float>(asset_->world_z());
}

float FramePointResolver::to_percent(float world_z) const {
    const float height = parent_height_px();
    if (height <= 0.0f) {
        return 0.0f;
    }
    const float base_z = base_world_z();
    return (world_z - base_z) / height;
}

float FramePointResolver::to_world_z(float z_percent) const {
    if (!asset_) {
        return 0.0f;
    }
    const float height = parent_height_px();
    const float base_z = base_world_z();
    return base_z + (z_percent * height);
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
    int source_height = source_asset->height();

    if (source_height > 0) {
        vals.dx_percent = static_cast<float>(x) / static_cast<float>(source_height);
        vals.dy_percent = static_cast<float>(y) / static_cast<float>(source_height);
        vals.dz_percent = static_cast<float>(z) / static_cast<float>(source_height);
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

    int source_height = source_asset->height();

    int displaced_x = source_gp->world_x() + static_cast<int>(vals.dx_percent * source_height);
    int displaced_y = source_gp->world_y() + static_cast<int>(vals.dy_percent * source_height);
    int displaced_z = source_gp->world_z() + static_cast<int>(vals.dz_percent * source_height);

    world::GridKey key{displaced_x, displaced_y, displaced_z, source_gp->resolution_layer()};
    return assets->world_grid().find_grid_point_strict(key);
}

}  // namespace devmode::frame_editors
