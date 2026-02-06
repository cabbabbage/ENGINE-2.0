#pragma once

#include <vector>

#include <SDL3/SDL.h>

class Asset;

class PathSanitizer {
public:
    std::vector<SDL_Point> sanitize(const Asset& self, const std::vector<SDL_Point>& absolute_checkpoints, int visited_thresh_px) const;
};
