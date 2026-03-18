#pragma once

#include <SDL3/SDL.h>
#include <vector>

class Asset;
class Assets;

class CompositeAssetRenderer {
public:
    CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets);
    ~CompositeAssetRenderer();

    void update(Asset* asset, float flicker_time_seconds = 0.0f);

private:
    // Keep these internal to composite packaging behavior.
    static constexpr float kDepthCueCenterDeadzonePx = 1.5f;
    static constexpr float kDepthCueMinDepthRangeMeters = 0.001f;

    void regenerate_package(Asset* asset, float flicker_time_seconds, float package_scale);
    void calculate_local_bounds(Asset* asset);

    SDL_Renderer* renderer_;
    Assets* assets_;
};
