#pragma once

#include <cstddef>
#include <SDL3/SDL.h>
#include <vector>

class Asset;
class Assets;
struct RenderObject;
struct DepthCueMergeSignature;
namespace depth_cue { struct DepthCueSettings; }

class CompositeAssetRenderer {
public:
    CompositeAssetRenderer(SDL_Renderer* renderer, Assets* assets);
    ~CompositeAssetRenderer();

    void update(Asset* asset, float flicker_time_seconds = 0.0f);

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
    DepthCueMergeSignature test_build_depth_cue_merge_signature(
        SDL_Texture* base_texture,
        SDL_Texture* foreground_texture,
        SDL_Texture* background_texture,
        float signed_depth,
        Uint8 base_alpha,
        bool depth_effects_enabled,
        const depth_cue::DepthCueSettings& settings) const;
    bool test_should_mark_composite_dirty_for_depth_cue_merge(
        const Asset* asset,
        const DepthCueMergeSignature& desired_signature) const;
#endif

private:
    void regenerate_package(Asset* asset, float flicker_time_seconds, float package_scale);
    DepthCueMergeSignature evaluate_depth_cue_merge_signature(Asset* asset) const;
    DepthCueMergeSignature build_depth_cue_merge_signature(
        SDL_Texture* base_texture,
        SDL_Texture* foreground_texture,
        SDL_Texture* background_texture,
        float signed_depth,
        Uint8 base_alpha,
        bool depth_effects_enabled,
        const depth_cue::DepthCueSettings& settings) const;
    bool should_mark_composite_dirty_for_depth_cue_merge(
        const Asset* asset,
        const DepthCueMergeSignature& desired_signature) const;
    SDL_Texture* compose_depth_cue_merged_texture(
        SDL_Texture* base_texture,
        const SDL_Rect* base_src_rect,
        int base_frame_w,
        int base_frame_h,
        SDL_Texture* overlay_texture,
        Uint8 overlay_alpha,
        Uint8 base_alpha,
        SDL_Point* out_size) const;
    void calculate_local_bounds(Asset* asset);

    SDL_Renderer* renderer_;
    Assets* assets_;
};
