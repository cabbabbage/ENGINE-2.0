#pragma once

#include <SDL3/SDL.h>

class Asset;
struct RenderObject;

namespace render_build {

struct DirectAssetRenderCacheRecord {
    SDL_Texture* texture = nullptr;
    int atlas_w = 0;
    int atlas_h = 0;
    bool has_atlas_size = false;
    bool has_src_rect = false;
    SDL_Rect src_rect{0, 0, 0, 0};
    int frame_w = 0;
    int frame_h = 0;
    bool has_texture_size = false;
    SDL_FPoint projection_anchor_uv{0.5f, 1.0f};
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    int frame_identity = -1;
    int variant_identity = -1;
    SDL_Texture* texture_identity = nullptr;
    Uint32 reprojection_identity = 0;
};

bool refresh_direct_asset_render_cache(Asset* asset, DirectAssetRenderCacheRecord& cache_record);
Uint32 direct_asset_reprojection_identity(Asset* asset);
bool build_direct_asset_render_object(Asset* asset,
                                      const DirectAssetRenderCacheRecord& cache_record,
                                      RenderObject& out_object);
bool build_direct_asset_render_object(Asset* asset, RenderObject& out_object);

}
