#pragma once

#include <vector>

#include "core/axis_convention.hpp"

class Asset;
struct CollisionQueryContext;

class PathSanitizer3D {
public:
    std::vector<axis::WorldPos> sanitize(const Asset& self,
                                         const std::vector<axis::WorldPos>& absolute_checkpoints,
                                         int visited_thresh_px,
                                         CollisionQueryContext* collision_context = nullptr) const;
};
