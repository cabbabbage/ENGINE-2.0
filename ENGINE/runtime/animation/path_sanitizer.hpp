#pragma once

#include <vector>

#include <SDL3/SDL.h>

#include "collision_query_context.hpp"

class Asset;

class PathSanitizer {
public:
    std::vector<SDL_Point> sanitize(const Asset& self,
                                    const std::vector<SDL_Point>& absolute_checkpoints,
                                    int visited_thresh_px,
                                    CollisionQueryContext* collision_context = nullptr) const;
};

#if defined(ENGINE_WORLD_TESTS)
namespace path_sanitizer::test_hooks {
bool checkpoint_collapses_to_anchor(SDL_Point anchor, SDL_Point candidate);
}
#endif
