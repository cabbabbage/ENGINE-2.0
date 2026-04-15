#pragma once

#include <SDL3/SDL.h>

#include "assets/asset/Asset.hpp"
#include "core/axis_convention.hpp"

namespace animation_update::movement_targets {

inline SDL_Point world_checkpoint(const Asset& target_asset) {
    return target_asset.world_xz_point();
}

inline SDL_Point world_delta_to_checkpoint(const Asset& self_asset, SDL_Point world_checkpoint) {
    return SDL_Point{
        world_checkpoint.x - self_asset.world_x(),
        world_checkpoint.y - self_asset.world_z()
    };
}

inline axis::WorldPos world_checkpoint_3d(const Asset& target_asset) {
    return axis::WorldPos{
        target_asset.world_x(),
        target_asset.world_y(),
        target_asset.world_z()
    };
}

inline axis::WorldPos world_delta_to_checkpoint_3d(const Asset& self_asset, const axis::WorldPos& world_checkpoint) {
    return axis::WorldPos{
        world_checkpoint.x - self_asset.world_x(),
        world_checkpoint.y - self_asset.world_y(),
        world_checkpoint.z - self_asset.world_z()
    };
}

} // namespace animation_update::movement_targets
