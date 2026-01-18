#pragma once

#include <SDL.h>
#include <vector>

class Asset;
class Assets;

class CompositeAssetRenderer {
public:
    // World-pixel distances used by the depth cue overlay blend.
    // Assets at or closer than the foreground distance render the foreground overlay at full opacity.
    // Assets at or farther than the background distance render the background overlay at full opacity.
    static constexpr float kDepthCueForegroundFullOpacityDistance = 900.0f;
    static constexpr float kDepthCueBackgroundFullOpacityDistance = 3200.0f;

    CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets);
    ~CompositeAssetRenderer();

    void update(Asset* asset, float flicker_time_seconds = 0.0f);

private:
    void regenerate_package(Asset* asset, float flicker_time_seconds, float package_scale);
    void calculate_local_bounds(Asset* asset);

    SDL_Renderer* renderer_;
    Assets* assets_;
};
