#pragma once

#include <SDL.h>
#include <vector>

class Asset;
class Assets;

class CompositeAssetRenderer {
public:
    CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets);
    ~CompositeAssetRenderer();

    void update(Asset* asset, float flicker_time_seconds = 0.0f);

private:
    void regenerate_package(Asset* asset, float flicker_time_seconds, float package_scale);
    void calculate_local_bounds(Asset* asset);

    SDL_Renderer* renderer_;
    Assets* assets_;
};
