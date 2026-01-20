#pragma once

#include <SDL.h>
#include <vector>

class Asset;
class Assets;

class CompositeAssetRenderer {
public:
    // Screen-pixel distances from center used by the depth cue overlay blend.
    // Assets at screen center render the foreground overlay at full opacity.
    // Assets at screen edges render the background overlay at full opacity.
    static constexpr float kDepthCueForegroundFullOpacityDistance = 1800.0f;
    static constexpr float kDepthCueBackgroundFullOpacityDistance = 1200.0f;

    CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets);
    ~CompositeAssetRenderer();

    void update(Asset* asset, float flicker_time_seconds = 0.0f);

private:
    void regenerate_package(Asset* asset, float flicker_time_seconds, float package_scale);
    void calculate_local_bounds(Asset* asset);

    SDL_Renderer* renderer_;
    Assets* assets_;
};
