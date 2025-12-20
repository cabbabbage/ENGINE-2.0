#include "world/chunk.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/light_source.hpp"
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
                                                      int world_z,
                                                      float static_weight,
                                                      float dynamic_weight,
                                                      float query_radius) const {
    std::scoped_lock lock(mutex_);
    SampledBrightness result{};
    const auto weights = resolve_sampling_weights(static_weight, dynamic_weight);

    if (!assets_) {
        result.static_component = result.dynamic_component = result.blended = 1.0f;
        result.color = SDL_Color{255, 255, 255, 255};
        result.has_color = false;
        return result;
    }

    world::WorldGrid& grid = assets_->world_grid();
    // Broad phase: query lights near the sample point within a generous radius based on max light radius.
    const float kFallbackRadius = (query_radius > 0.0f) ? query_radius : 512.0f;
    SDL_FRect query_bounds{
        static_cast<float>(world_x) - kFallbackRadius,
        static_cast<float>(world_y) - kFallbackRadius,
        kFallbackRadius * 2.0f,
        kFallbackRadius * 2.0f
};

    auto lights = grid.query_lights(query_bounds, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), /*skip_inactive_branches=*/true);
    float max_component = 0.0f;
    SDL_Color accum_color{0, 0, 0, 0};
    int color_contribs = 0;

    for (const auto& light : lights) {
        if (!light.source || !light.point) continue;
        const int radius_px = std::max(1, light.source->radius);
        const float lx = static_cast<float>(light.point->world_x() + light.source->offset_x);
        const float ly = static_cast<float>(light.point->world_y() + light.source->offset_y);
        const float lz = static_cast<float>(light.point->world_z());
        const float dx = static_cast<float>(world_x) - lx;
        const float dy = static_cast<float>(world_y) - ly;
        const float dz = static_cast<float>(world_z) - lz;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > static_cast<float>(radius_px)) {
            continue;
        }
        const float intensity = static_cast<float>(std::clamp(light.source->intensity, 0, 255)) / 255.0f;
        const float falloff = 1.0f - (dist / static_cast<float>(radius_px));
        const float contribution = std::clamp(intensity * falloff, 0.0f, 1.0f);
        max_component = std::max(max_component, contribution);

        auto accumulate_color = [&](Uint8 channel, float weight) -> Uint8 {
            const int val = static_cast<int>(std::lround(static_cast<float>(channel) * weight * 255.0f));
            return static_cast<Uint8>(std::clamp(val, 0, 255));
};
        accum_color.r = static_cast<Uint8>(std::min(255, static_cast<int>(accum_color.r) + accumulate_color(light.source->color.r, contribution)));
        accum_color.g = static_cast<Uint8>(std::min(255, static_cast<int>(accum_color.g) + accumulate_color(light.source->color.g, contribution)));
        accum_color.b = static_cast<Uint8>(std::min(255, static_cast<int>(accum_color.b) + accumulate_color(light.source->color.b, contribution)));
        accum_color.a = 255;
        ++color_contribs;
    }

    const float base_component = 1.0f;
    result.static_component = std::clamp(base_component + max_component, 0.0f, 1.0f);
    result.dynamic_component = result.static_component;
    result.blended = result.static_component;
    result.has_color = color_contribs > 0;
    result.color = result.has_color ? accum_color : SDL_Color{255, 255, 255, 255};
    return result;
}

LightMap::SampledBrightness LightMap::sample_lighting_bilinear(float world_x,
                                                               float world_y,
                                                               float world_z,
                                                               float static_weight,
                                                               float dynamic_weight,
                                                               float query_radius) const {
    return sample_lighting(static_cast<int>(std::lround(world_x)),
                           static_cast<int>(std::lround(world_y)),
                           static_cast<int>(std::lround(world_z)),
                           static_weight,
                           dynamic_weight,
                           query_radius);
}

float LightMap::sample_brightness(int world_x,
                                  int world_y,
                                  int world_z,
                                  float static_weight,
                                  float dynamic_weight,
                                  float query_radius) const {
    return sample_lighting(world_x, world_y, world_z, static_weight, dynamic_weight, query_radius).blended;
}

float LightMap::sample_brightness_bilinear(float world_x,
                                           float world_y,
                                           float world_z,
                                           float static_weight,
                                           float dynamic_weight,
                                           float query_radius) const {
    return sample_lighting_bilinear(world_x, world_y, world_z, static_weight, dynamic_weight, query_radius).blended;
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

