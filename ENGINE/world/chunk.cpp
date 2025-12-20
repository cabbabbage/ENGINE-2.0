#include "world/chunk.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "render/warped_screen_grid.hpp"
#include "render/render.hpp"
#include "world/world_grid.hpp"

namespace world {

Chunk::Chunk(int in_i, int in_j, int r, SDL_Rect bounds)
    : i(in_i)
    , j(in_j)
    , r_chunk(r)
    , world_bounds(bounds) {}

Chunk::~Chunk() {
    releaseTileTextures();
}
void Chunk::releaseTileTextures() {
    for (auto& t : tiles) {
        if (t.texture) {
            SDL_DestroyTexture(t.texture);
            t.texture = nullptr;
        }
    }
    tiles.clear();
}

}

LightMap::LightMap(Assets* assets, int screen_width, int screen_height)
    : assets_(assets)
    , screen_width_(screen_width)
    , screen_height_(screen_height) {}

LightMap::~LightMap() = default;

void LightMap::rebuild(SDL_Renderer*) {
    std::scoped_lock lock(mutex_);
    (void)assets_;
}

void LightMap::update(SDL_Renderer*, std::uint32_t) {
    std::scoped_lock lock(mutex_);
    (void)assets_;
}

LightMap::SampledBrightness LightMap::sample_lighting(int world_x,
                                                      int world_y,
                                                      float static_weight,
                                                      float dynamic_weight) const {
    std::scoped_lock lock(mutex_);
    SampledBrightness result{};
    const auto weights = resolve_sampling_weights(static_weight, dynamic_weight);

    const float base_component = 1.0f;
    result.static_component = base_component;
    result.dynamic_component = base_component;
    result.has_color = false;
    result.color = SDL_Color{255, 255, 255, 255};

    const float total_weight = weights.first + weights.second;
    if (total_weight <= 1e-6f) {
        result.blended = base_component;
    } else {
        const float weighted_total = (base_component * weights.first + base_component * weights.second) / total_weight;
        result.blended = std::clamp(weighted_total, 0.0f, 1.0f);
    }

    (void)world_x;
    (void)world_y;
    return result;
}

LightMap::SampledBrightness LightMap::sample_lighting_bilinear(float world_x,
                                                               float world_y,
                                                               float static_weight,
                                                               float dynamic_weight) const {
    return sample_lighting(world_x, world_y, static_weight, dynamic_weight);
}

float LightMap::sample_brightness(int world_x,
                                  int world_y,
                                  float static_weight,
                                  float dynamic_weight) const {
    return sample_lighting(world_x, world_y, static_weight, dynamic_weight).blended;
}

float LightMap::sample_brightness_bilinear(float world_x,
                                           float world_y,
                                           float static_weight,
                                           float dynamic_weight) const {
    return sample_lighting_bilinear(world_x, world_y, static_weight, dynamic_weight).blended;
}

void LightMap::render_visible_chunks(SDL_Renderer* renderer, const SDL_Rect& view_rect) const {
    static constexpr SDL_Color kDefaultColor{0, 0, 0, 255};
    render_visible_chunks(renderer, view_rect, 1.0f, kDefaultColor);
}

void LightMap::render_visible_chunks(SDL_Renderer* renderer,
                                     const SDL_Rect& view_rect,
                                     float           alpha_multiplier,
                                     const SDL_Color& color_mod) const {
    std::scoped_lock lock(mutex_);
    (void)renderer;
    (void)view_rect;
    (void)alpha_multiplier;
    (void)color_mod;
}

void LightMap::subtract_runtime_shadow_from_texture(SDL_Renderer* renderer,
                                                    SDL_Texture*  target_texture,
                                                    const SDL_Rect& target_rect,
                                                    const SDL_Rect& screen_rect,
                                                    float           alpha_multiplier) const {
    if (!renderer || !target_texture) {
        return;
    }

    (void)renderer;
    (void)target_texture;
    (void)target_rect;
    (void)screen_rect;
    (void)alpha_multiplier;
}

void LightMap::render_chunk_preview(SDL_Renderer* renderer, const SDL_Rect& view_rect) const {
    std::scoped_lock lock(mutex_);
    (void)renderer;
    (void)view_rect;
}

void LightMap::present_static_previews(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }
    SDL_RenderPresent(renderer);
}

void LightMap::mark_region_dirty(const SDL_Rect&) {}

void LightMap::mark_asset_lights_dirty(const Asset*) {}

void LightMap::mark_static_cache_dirty() {}

const std::vector<world::Chunk*>& LightMap::active_chunks() const {
    static const std::vector<world::Chunk*> kEmpty;
    if (!assets_) {
        return kEmpty;
    }
    return assets_->world_grid().active_chunks();
}

world::Chunk* LightMap::ensure_chunk_from_world(SDL_Point world_px) const {
    if (!assets_) {
        return nullptr;
    }
    return assets_->world_grid().ensure_chunk_from_world(world_px);
}

world::Chunk* LightMap::chunk_from_world(SDL_Point world_px) const {
    if (!assets_) {
        return nullptr;
    }
    return assets_->world_grid().chunk_from_world(world_px);
}

int LightMap::chunk_count() const {
    if (!assets_) {
        return 0;
    }
    return static_cast<int>(assets_->world_grid().chunks().storage().size());
}

int LightMap::chunk_columns() const {
    if (!assets_) {
        return 0;
    }
    const auto& chunks = assets_->world_grid().chunks().storage();
    if (chunks.empty()) {
        return 0;
    }
    int min_i = chunks.front()->i;
    int max_i = chunks.front()->i;
    for (const auto& chunk : chunks) {
        if (!chunk) {
            continue;
        }
        min_i = std::min(min_i, chunk->i);
        max_i = std::max(max_i, chunk->i);
    }
    return (max_i - min_i) + 1;
}

int LightMap::chunk_rows() const {
    if (!assets_) {
        return 0;
    }
    const auto& chunks = assets_->world_grid().chunks().storage();
    if (chunks.empty()) {
        return 0;
    }
    int min_j = chunks.front()->j;
    int max_j = chunks.front()->j;
    for (const auto& chunk : chunks) {
        if (!chunk) {
            continue;
        }
        min_j = std::min(min_j, chunk->j);
        max_j = std::max(max_j, chunk->j);
    }
    return (max_j - min_j) + 1;
}

const world::Chunk* LightMap::chunk_at(int index) const {
    if (!assets_ || index < 0) {
        return nullptr;
    }
    const auto& chunks = assets_->world_grid().chunks().storage();
    if (index >= static_cast<int>(chunks.size())) {
        return nullptr;
    }
    return chunks[static_cast<std::size_t>(index)].get();
}

SDL_Rect LightMap::chunk_bounds(int index) const {
    if (const world::Chunk* chunk = chunk_at(index)) {
        return chunk->world_bounds;
    }
    return SDL_Rect{0, 0, 0, 0};
}

std::pair<float, float> LightMap::resolve_sampling_weights(float static_weight, float dynamic_weight) const {
    const float base_static  = 0.0f;
    const float base_dynamic = 1.0f;

    float effective_static  = static_weight;
    float effective_dynamic = dynamic_weight;

    if (std::abs(static_weight - base_static) <= 1e-4f) {
        effective_static = base_static;
    }
    if (std::abs(dynamic_weight - base_dynamic) <= 1e-4f) {
        effective_dynamic = base_dynamic;
    }

    return {std::max(0.0f, effective_static), std::max(0.0f, effective_dynamic)};
}

