#pragma once

#include <SDL3/SDL.h>

#include "animation.hpp"

class AssetInfo;

class AnimationCloner {
public:
    struct Options {
        bool flip_horizontal = false;
        bool flip_vertical   = false;
        bool reverse_frames  = false;
        bool invert_movement_x = false;
        bool invert_movement_y = false;
        bool invert_movement_z = false;
        bool inherit_on_end_from_source = false;
};

    static bool Clone(const Animation& source, Animation&       dest, const Options&   opts, SDL_Renderer*    renderer, AssetInfo&       info);
};
