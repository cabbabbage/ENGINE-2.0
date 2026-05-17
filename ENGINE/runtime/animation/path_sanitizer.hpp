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


