#pragma once

#include <SDL3/SDL.h>

#include "assets/asset/Asset.hpp"

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

} // namespace animation_update::movement_targets
