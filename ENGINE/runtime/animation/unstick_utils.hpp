#pragma once

#include <vector>

#include <SDL3/SDL.h>

#include "core/AssetsManager.hpp"
#include "gameplay/world/grid_point.hpp"

class Asset;

namespace animation::unstick {

using CollisionEntryRef = const Assets::FrameCollisionEntry*;

bool resolve_destination(const Asset& self,
                         const Assets* assets,
                         const std::vector<CollisionEntryRef>& entries,
                         const world::GridPoint& start,
                         world::GridPoint& out_destination,
                         int max_steps = 12);

bool push_out_of_impassable(Asset& self,
                            const Assets* assets,
                            const std::vector<CollisionEntryRef>& entries,
                            int max_steps = 12);

} // namespace animation::unstick
