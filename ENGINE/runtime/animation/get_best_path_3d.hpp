#pragma once

#include <vector>

#include "collision_query_context.hpp"
#include "core/axis_convention.hpp"
#include "stride_types.hpp"
#include "utils/grid.hpp"

class Asset;

class GetBestPath3D {
public:
    Plan3D operator()(const Asset& self,
                      const std::vector<axis::WorldPos>& sanitized_checkpoints,
                      int visited_thresh_px,
                      const vibble::grid::Grid& grid,
                      CollisionQueryContext* collision_context = nullptr) const;
};

namespace get_best_path_3d::test_hooks {

struct AnimationTagBuckets3D {
    std::vector<std::string> locomotion_animation_ids;
    std::vector<std::string> attack_animation_ids;
};

AnimationTagBuckets3D classify_animation_tag_buckets(const Asset& self);

} // namespace get_best_path_3d::test_hooks
