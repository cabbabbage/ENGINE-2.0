#pragma once

#include <vector>

#include <SDL3/SDL.h>

#include "collision_query_context.hpp"
#include "stride_types.hpp"
#include "utils/grid.hpp"

class Asset;

class GetBestPath {
public:
    Plan operator()(const Asset& self,
                    const std::vector<SDL_Point>& sanitized_checkpoints,
                    int visited_thresh_px,
                    const vibble::grid::Grid& grid,
                    CollisionQueryContext* collision_context = nullptr) const;
};

namespace get_best_path::test_hooks {

struct AnimationTagBuckets {
    std::vector<std::string> locomotion_animation_ids;
    std::vector<std::string> attack_animation_ids;
};

AnimationTagBuckets classify_animation_tag_buckets(const Asset& self);

} // namespace get_best_path::test_hooks
