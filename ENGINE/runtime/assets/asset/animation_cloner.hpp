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
};

    static bool Clone(const Animation& source, Animation&       dest, const Options&   opts, SDL_Renderer*    renderer, AssetInfo&       info);
};
