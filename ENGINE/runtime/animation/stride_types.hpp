#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "core/axis_convention.hpp"

struct Stride {
    std::string animation_id;
    int         frames = 0;
    std::size_t path_index = 0;
};

struct MovementTagFilter {
    std::vector<std::string> required_tags;
    std::vector<std::string> excluded_tags;
};

struct Plan {
    std::vector<SDL_Point> sanitized_checkpoints;
    std::vector<Stride>    strides;
    SDL_Point              final_dest{0, 0};
    SDL_Point              world_start{0, 0};
    std::optional<std::string> engagement_target_asset_id{};
    bool                   override_non_locked = true;
    bool                   attacking_enabled = false;
    MovementTagFilter      movement_tag_filter{};
};

struct Plan3D {
    std::vector<axis::WorldPos> sanitized_checkpoints;
    std::vector<Stride>         strides;
    axis::WorldPos              final_dest{0, 0, 0};
    axis::WorldPos              world_start{0, 0, 0};
    std::optional<std::string>  engagement_target_asset_id{};
    bool                        override_non_locked = true;
    bool                        attacking_enabled = false;
    MovementTagFilter           movement_tag_filter{};
};
