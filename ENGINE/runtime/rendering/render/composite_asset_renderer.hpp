#pragma once

#include <cstddef>
#include <SDL3/SDL.h>
#include <vector>

class Asset;
class Assets;
struct RenderObject;

class CompositeAssetRenderer {
public:
    CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets);
    ~CompositeAssetRenderer();

    void update(Asset* asset, float flicker_time_seconds = 0.0f);

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
    bool test_upsert_depth_cue_overlay_object(Asset* asset,
                                              std::size_t base_index,
                                              const RenderObject& desired_overlay) {
        return upsert_depth_cue_overlay_object(asset, base_index, desired_overlay);
    }
    bool test_remove_depth_cue_overlay_objects(Asset* asset) {
        return remove_depth_cue_overlay_objects(asset);
    }
#endif

private:
    void regenerate_package(Asset* asset, float flicker_time_seconds, float package_scale);
    bool refresh_depth_cue_overlay(Asset* asset);
    bool upsert_depth_cue_overlay_object(Asset* asset, std::size_t base_index, const RenderObject& desired_overlay);
    bool remove_depth_cue_overlay_objects(Asset* asset);
    void calculate_local_bounds(Asset* asset);

    SDL_Renderer* renderer_;
    Assets* assets_;
};
