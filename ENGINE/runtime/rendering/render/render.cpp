#include "rendering/render/render.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <utility>
#include <vector>

#include <SDL3_image/SDL_image.h>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_library.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/dynamic_boundary_system.hpp"
#include "rendering/render/projected_sprite_frame.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "animation/animation_update.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "animation/controllers/shared/anchored_child_placement.hpp"
#include "gameplay/world/tiling/grid_tile.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "utils/AnchorPointResolver.hpp"
#include "utils/log.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"

namespace {
constexpr double kDepthBucketSize = 0.0625;
constexpr double kDepthBucketScale = 1.0 / kDepthBucketSize;
constexpr int kMotionBlurHistoryFrameCount = 2;
constexpr std::array<Uint8, kMotionBlurHistoryFrameCount> kMotionBlurHistoryAlpha = {120, 60};
constexpr bool kSceneMotionBlurEnabled = false;
// Runtime casted shadows removed. Lights are additive only.
constexpr bool kRuntimeCastedShadowsEnabled = false;

inline std::int64_t quantize_depth(double depth) {
    const double scaled = std::floor(depth * kDepthBucketScale);
    const double min_value = static_cast<double>(std::numeric_limits<std::int64_t>::lowest());
    const double max_value = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    const double clamped = std::clamp(scaled, min_value, max_value);
    return static_cast<std::int64_t>(clamped);
}

bool enforce_trapezoid(std::array<SDL_FPoint, 4>& points);

float point_segment_distance_sq(const SDL_FPoint& point,
                                const SDL_FPoint& seg_a,
                                const SDL_FPoint& seg_b) {
    const float vx = seg_b.x - seg_a.x;
    const float vy = seg_b.y - seg_a.y;
    const float wx = point.x - seg_a.x;
    const float wy = point.y - seg_a.y;
    const float denom = (vx * vx) + (vy * vy);
    if (denom <= 1e-5f) {
        const float dx = point.x - seg_a.x;
        const float dy = point.y - seg_a.y;
        return (dx * dx) + (dy * dy);
    }
    const float t = std::clamp(((wx * vx) + (wy * vy)) / denom, 0.0f, 1.0f);
    const float cx = seg_a.x + (vx * t);
    const float cy = seg_a.y + (vy * t);
    const float dx = point.x - cx;
    const float dy = point.y - cy;
    return (dx * dx) + (dy * dy);
}

float min_distance_sq_to_quad_edges(const std::array<SDL_FPoint, 4>& quad,
                                    const SDL_FPoint& point) {
    float best = std::numeric_limits<float>::max();
    for (int i = 0; i < 4; ++i) {
        const SDL_FPoint& a = quad[static_cast<std::size_t>(i)];
        const SDL_FPoint& b = quad[static_cast<std::size_t>((i + 1) % 4)];
        best = std::min(best, point_segment_distance_sq(point, a, b));
    }
    return best;
}

bool point_inside_convex_quad(const std::array<SDL_FPoint, 4>& quad,
                              const SDL_FPoint& point) {
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const SDL_FPoint& a = quad[static_cast<std::size_t>(i)];
        const SDL_FPoint& b = quad[static_cast<std::size_t>((i + 1) % 4)];
        const float value = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
        if (std::fabs(value) <= 1e-5f) {
            continue;
        }
        if (sign == 0.0f) {
            sign = value;
        } else if ((sign > 0.0f) != (value > 0.0f)) {
            return false;
        }
    }
    return true;
}

float quad_area(const std::array<SDL_FPoint, 4>& quad) {
    float area = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const SDL_FPoint& a = quad[static_cast<std::size_t>(i)];
        const SDL_FPoint& b = quad[static_cast<std::size_t>((i + 1) % 4)];
        area += (a.x * b.y) - (b.x * a.y);
    }
    return std::fabs(area) * 0.5f;
}

struct GeometryBatcherDepthBucketCache {
    const GeometryBatcher* batcher = nullptr;
    std::int64_t quantized_depth = 0;
    void* bucket = nullptr;
};

GeometryBatcherDepthBucketCache& geometry_batcher_depth_bucket_cache() {
    static thread_local GeometryBatcherDepthBucketCache cache;
    return cache;
}

struct CachedGroundTileProjection {
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Rect world_rect{0, 0, 0, 0};
    std::uint64_t camera_state_version = 0;
    std::array<SDL_Vertex, 4> vertices{};
    bool valid = false;
};

bool cached_ground_tile_projection_matches(const CachedGroundTileProjection& entry,
                                           SDL_Renderer* renderer,
                                           SDL_Texture* texture,
                                           const SDL_Rect& world_rect,
                                           std::uint64_t camera_state_version) {
    return entry.valid &&
           entry.renderer == renderer &&
           entry.texture == texture &&
           entry.camera_state_version == camera_state_version &&
           entry.world_rect.x == world_rect.x &&
           entry.world_rect.y == world_rect.y &&
           entry.world_rect.w == world_rect.w &&
           entry.world_rect.h == world_rect.h;
}

std::unordered_map<const void*, CachedGroundTileProjection>& ground_tile_projection_cache() {
    static std::unordered_map<const void*, CachedGroundTileProjection> cache;
    return cache;
}

void maybe_prune_ground_tile_projection_cache() {
    auto& cache = ground_tile_projection_cache();
    constexpr std::size_t kMaxCachedGroundTiles = 65536;
    if (cache.size() > kMaxCachedGroundTiles) {
        cache.clear();
    }
}

struct SpatialCellKey {
    int x = 0;
    int y = 0;

    bool operator==(const SpatialCellKey& other) const noexcept {
        return x == other.x && y == other.y;
    }
};

struct SpatialCellKeyHash {
    std::size_t operator()(const SpatialCellKey& key) const noexcept {
        const std::uint64_t ux = static_cast<std::uint32_t>(key.x);
        const std::uint64_t uy = static_cast<std::uint32_t>(key.y);
        return static_cast<std::size_t>((ux << 32) ^ uy);
    }
};

using SpatialIndex = std::unordered_map<SpatialCellKey, std::vector<std::size_t>, SpatialCellKeyHash>;

int spatial_cell_coord(float value, float cell_size) {
    if (!std::isfinite(value) || cell_size <= 0.0f) {
        return 0;
    }
    return static_cast<int>(std::floor(value / cell_size));
}

SpatialCellKey make_spatial_cell_key(const SDL_FPoint& point, float cell_size) {
    return SpatialCellKey{
        spatial_cell_coord(point.x, cell_size),
        spatial_cell_coord(point.y, cell_size)
    };
}

template <typename PointGetter>
void build_spatial_index(std::size_t item_count,
                         float cell_size,
                         PointGetter&& point_getter,
                         SpatialIndex& out_index) {
    out_index.clear();
    if (item_count == 0 || cell_size <= 0.0f) {
        return;
    }

    out_index.reserve(item_count * 2);
    for (std::size_t i = 0; i < item_count; ++i) {
        const SDL_FPoint point = point_getter(i);
        out_index[make_spatial_cell_key(point, cell_size)].push_back(i);
    }
}

template <typename Fn>
void for_each_spatial_candidate(const SpatialIndex& index,
                                const SDL_FPoint& point,
                                float radius,
                                float cell_size,
                                Fn&& fn) {
    if (index.empty() || cell_size <= 0.0f) {
        return;
    }

    const int min_cell_x = spatial_cell_coord(point.x - radius, cell_size);
    const int max_cell_x = spatial_cell_coord(point.x + radius, cell_size);
    const int min_cell_y = spatial_cell_coord(point.y - radius, cell_size);
    const int max_cell_y = spatial_cell_coord(point.y + radius, cell_size);

    for (int cell_y = min_cell_y; cell_y <= max_cell_y; ++cell_y) {
        for (int cell_x = min_cell_x; cell_x <= max_cell_x; ++cell_x) {
            const auto it = index.find(SpatialCellKey{cell_x, cell_y});
            if (it == index.end()) {
                continue;
            }
            for (std::size_t candidate_index : it->second) {
                fn(candidate_index);
            }
        }
    }
}

constexpr bool kBackgroundLayerFogEnabled = true;

bool project_depth_to_floor_screen_y(const WarpedScreenGrid& cam,
                                     double world_depth,
                                     int screen_height,
                                     float& out_y) {
    SDL_FPoint projected{};
    if (!cam.project_world_point(SDL_FPoint{0.0f, 0.0f}, static_cast<float>(world_depth), projected)) {
        return false;
    }
    if (!std::isfinite(projected.x) || !std::isfinite(projected.y)) {
        return false;
    }
    projected.y = cam.warp_floor_screen_y(0.0f, projected.y);
    if (!std::isfinite(projected.y)) {
        return false;
    }
    out_y = std::clamp(projected.y, 0.0f, static_cast<float>(screen_height));
    return true;
}


}

// ============================================================================
// GeometryBatcher Implementation
// ============================================================================

GeometryBatcher::GeometryBatcher(SDL_Renderer* renderer)
    : renderer_(renderer) {
    // Pre-allocate for estimated 10,000 quads (40,000 vertices, 60,000 indices)
    vertex_buffer_.reserve(40000);
    index_buffer_.reserve(60000);
}

void GeometryBatcher::addQuad(SDL_Texture* texture, const SDL_Vertex vertices[4],
                               const int indices[6], SDL_BlendMode blend_mode, double depth) {
    if (!texture) return;
    (void)indices; // indices are standardized for quads

    DrawItem item{};
    item.texture = texture;
    item.blend_mode = blend_mode;
    item.vertices[0] = vertices[0];
    item.vertices[1] = vertices[1];
    item.vertices[2] = vertices[2];
    item.vertices[3] = vertices[3];
    item.depth = depth;

    DepthBucket* bucket = nullptr;
    auto& bucket_cache = geometry_batcher_depth_bucket_cache();
    if (std::isfinite(depth)) {
        const auto quantized_depth = quantize_depth(depth);
        if (bucket_cache.batcher == this &&
            bucket_cache.bucket != nullptr &&
            bucket_cache.quantized_depth == quantized_depth) {
            bucket = static_cast<DepthBucket*>(bucket_cache.bucket);
        } else {
            bucket = &depth_buckets_[quantized_depth];
            bucket_cache.batcher = this;
            bucket_cache.quantized_depth = quantized_depth;
            bucket_cache.bucket = bucket;
        }
    } else {
        bucket = &invalid_depth_bucket_;
        if (bucket_cache.batcher == this) {
            bucket_cache.bucket = nullptr;
        }
    }

    bucket->items.push_back(item);
}

void GeometryBatcher::flush() {
    if (depth_buckets_.empty() && invalid_depth_bucket_.items.empty()) {
        last_flush_cpu_ms_ = 0.0;
        return;
    }

    const auto flush_start = std::chrono::steady_clock::now();

    draw_call_count_ = 0;
    total_vertices_ = 0;

    // Static quad indices pattern (0,1,2, 0,2,3)
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    vertex_buffer_.clear();
    index_buffer_.clear();

    auto emit_current_batch = [&](SDL_Texture* current_texture, SDL_BlendMode current_blend) {
        if (!current_texture || vertex_buffer_.empty() || index_buffer_.empty()) {
            return;
        }
        SDL_SetTextureBlendMode(current_texture, current_blend);
        SDL_RenderGeometry(renderer_,
                           current_texture,
                           vertex_buffer_.data(),
                           static_cast<int>(vertex_buffer_.size()),
                           index_buffer_.data(),
                           static_cast<int>(index_buffer_.size()));

        ++draw_call_count_;
        total_vertices_ += vertex_buffer_.size();

        vertex_buffer_.clear();
        index_buffer_.clear();
    };

    auto emit_item = [&](const DrawItem& quad, SDL_Texture*& current_texture, SDL_BlendMode& current_blend) {
        if (quad.texture != current_texture || quad.blend_mode != current_blend) {
            emit_current_batch(current_texture, current_blend);
            current_texture = quad.texture;
            current_blend = quad.blend_mode;
        }

        const int base_index = static_cast<int>(vertex_buffer_.size());

        vertex_buffer_.push_back(quad.vertices[0]);
        vertex_buffer_.push_back(quad.vertices[1]);
        vertex_buffer_.push_back(quad.vertices[2]);
        vertex_buffer_.push_back(quad.vertices[3]);

        index_buffer_.push_back(base_index + kQuadIndices[0]);
        index_buffer_.push_back(base_index + kQuadIndices[1]);
        index_buffer_.push_back(base_index + kQuadIndices[2]);
        index_buffer_.push_back(base_index + kQuadIndices[3]);
        index_buffer_.push_back(base_index + kQuadIndices[4]);
        index_buffer_.push_back(base_index + kQuadIndices[5]);
    };

    SDL_Texture* current_texture = nullptr;
    SDL_BlendMode current_blend = SDL_BLENDMODE_BLEND;

    for (auto bucket_it = depth_buckets_.rbegin(); bucket_it != depth_buckets_.rend(); ++bucket_it) {
        const DepthBucket& bucket = bucket_it->second;
        for (const DrawItem& item : bucket.items) {
            emit_item(item, current_texture, current_blend);
        }
    }

    for (const DrawItem& item : invalid_depth_bucket_.items) {
        emit_item(item, current_texture, current_blend);
    }

    emit_current_batch(current_texture, current_blend);

    const auto flush_end = std::chrono::steady_clock::now();
    last_flush_cpu_ms_ = std::chrono::duration<double, std::milli>(flush_end - flush_start).count();
}

void GeometryBatcher::clear() {
    depth_buckets_.clear();
    invalid_depth_bucket_ = DepthBucket{};
    draw_call_count_ = 0;
    total_vertices_ = 0;
    last_flush_cpu_ms_ = 0.0;
    vertex_buffer_.clear();
    index_buffer_.clear();

    auto& bucket_cache = geometry_batcher_depth_bucket_cache();
    if (bucket_cache.batcher == this) {
        bucket_cache = GeometryBatcherDepthBucketCache{};
    }
}

void GeometryBatcher::for_each_item_far_to_near(const std::function<void(const DrawItem&)>& fn) const {
    if (!fn) {
        return;
    }
    for (auto bucket_it = depth_buckets_.rbegin(); bucket_it != depth_buckets_.rend(); ++bucket_it) {
        for (const DrawItem& item : bucket_it->second.items) {
            fn(item);
        }
    }
    for (const DrawItem& item : invalid_depth_bucket_.items) {
        fn(item);
    }
}

void GridTileRenderer::invalidate_texture_cache() {
    texture_size_cache_.clear();
}

bool GridTileRenderer::fetch_texture_size(SDL_Texture* texture, SDL_FPoint& out_size) {
    if (!texture) {
        return false;
    }
    auto it = texture_size_cache_.find(texture);
    if (it != texture_size_cache_.end()) {
        out_size = it->second;
        return true;
    }

    float tex_wf = 0.0f;
    float tex_hf = 0.0f;
    if (!SDL_GetTextureSize(texture, &tex_wf, &tex_hf)) {
        return false;
    }

    const float rounded_w = static_cast<float>(std::lround(tex_wf));
    const float rounded_h = static_cast<float>(std::lround(tex_hf));
    if (rounded_w <= 0.0f || rounded_h <= 0.0f) {
        return false;
    }

    SDL_FPoint dims{rounded_w, rounded_h};
    texture_size_cache_.emplace(texture, dims);
    out_size = dims;
    return true;
}

void GridTileRenderer::render(SDL_Renderer* renderer) {
    if (!renderer || !assets_) return;
    render(renderer, assets_->getView(), assets_->world_grid(), nullptr);
}

void GridTileRenderer::render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid) {
    render(renderer, cam, grid, nullptr);
}

void GridTileRenderer::render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid, GeometryBatcher* batcher) {
    if (!renderer) return;

    const auto& chunks = grid.active_chunks();
    if (chunks.empty()) return;

    maybe_prune_ground_tile_projection_cache();
    auto& tile_cache = ground_tile_projection_cache();
    const std::uint64_t camera_state_version = cam.camera_state_version();

    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    int indices[6] = {0, 1, 2, 0, 2, 3};

    auto floor_project = [&](SDL_Point world_pos, SDL_FPoint& out) -> bool {
        SDL_FPoint screen{};
        const float world_depth = static_cast<float>(world_pos.y);
        if (!cam.project_world_point(SDL_FPoint{static_cast<float>(world_pos.x), 0.0f}, world_depth, screen)) {
            return false;
        }
        if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) {
            return false;
        }
        screen.y = cam.warp_floor_screen_y(0.0f, screen.y);
        if (!std::isfinite(screen.y)) {
            return false;
        }
        out = SDL_FPoint{std::floor(screen.x), std::floor(screen.y)};
        return true;
    };

    for (const world::Chunk* chunk : chunks) {
        if (!chunk) continue;
        for (const auto& tile : chunk->tiles) {
            if (!tile.texture || tile.world_rect.w <= 0 || tile.world_rect.h <= 0) continue;

            const void* tile_key = static_cast<const void*>(&tile);
            auto cache_it = tile_cache.find(tile_key);
            const bool can_reuse_cached_projection =
                cache_it != tile_cache.end() &&
                cached_ground_tile_projection_matches(cache_it->second,
                                                      renderer,
                                                      tile.texture,
                                                      tile.world_rect,
                                                      camera_state_version);

            SDL_Vertex verts[4]{};
            if (can_reuse_cached_projection) {
                verts[0] = cache_it->second.vertices[0];
                verts[1] = cache_it->second.vertices[1];
                verts[2] = cache_it->second.vertices[2];
                verts[3] = cache_it->second.vertices[3];
            } else {
                SDL_Point world_tl{ tile.world_rect.x, tile.world_rect.y };
                SDL_Point world_tr{ tile.world_rect.x + tile.world_rect.w, tile.world_rect.y };
                SDL_Point world_br{ tile.world_rect.x + tile.world_rect.w, tile.world_rect.y + tile.world_rect.h };
                SDL_Point world_bl{ tile.world_rect.x, tile.world_rect.y + tile.world_rect.h };

                SDL_FPoint screen_tl{};
                SDL_FPoint screen_tr{};
                SDL_FPoint screen_br{};
                SDL_FPoint screen_bl{};
                if (!floor_project(world_tl, screen_tl) ||
                    !floor_project(world_tr, screen_tr) ||
                    !floor_project(world_br, screen_br) ||
                    !floor_project(world_bl, screen_bl)) {
                    continue;
                }

                std::array<SDL_FPoint, 4> tile_points{screen_tl, screen_tr, screen_br, screen_bl};
                enforce_trapezoid(tile_points);
                screen_tl = tile_points[0];
                screen_tr = tile_points[1];
                screen_br = tile_points[2];
                screen_bl = tile_points[3];

                SDL_FPoint tex_size{};
                if (!fetch_texture_size(tile.texture, tex_size)) {
                    continue;
                }
                const float tex_w = tex_size.x;
                const float tex_h = tex_size.y;
                if (tex_w <= 0.0f || tex_h <= 0.0f) {
                    continue;
                }

                verts[0].position = screen_tl;
                verts[1].position = screen_tr;
                verts[2].position = screen_br;
                verts[3].position = screen_bl;
                for (auto& v : verts) {
                    v.color = white;
                }
                verts[0].tex_coord = SDL_FPoint{0.0f, 0.0f};
                verts[1].tex_coord = SDL_FPoint{tex_w, 0.0f};
                verts[2].tex_coord = SDL_FPoint{tex_w, tex_h};
                verts[3].tex_coord = SDL_FPoint{0.0f, tex_h};

                CachedGroundTileProjection& cache_entry = tile_cache[tile_key];
                cache_entry.renderer = renderer;
                cache_entry.texture = tile.texture;
                cache_entry.world_rect = tile.world_rect;
                cache_entry.camera_state_version = camera_state_version;
                cache_entry.vertices[0] = verts[0];
                cache_entry.vertices[1] = verts[1];
                cache_entry.vertices[2] = verts[2];
                cache_entry.vertices[3] = verts[3];
                cache_entry.valid = true;
            }

            if (batcher) {
                // Ground tiles sit behind world assets; use a large depth so batch order stays monotonic.
                batcher->addQuad(tile.texture, verts, indices, SDL_BLENDMODE_BLEND, 1'000'000.0);
            } else {
                SDL_RenderGeometry(renderer, tile.texture, verts, 4, indices, 6);
            }
        }
    }

}

namespace {

inline float ticks_to_seconds(Uint64 ticks) {
    return static_cast<float>(ticks) * 0.001f;
}

inline float smoothstep(float edge0, float edge1, float x) {
    if (edge0 == edge1) {
        return x < edge0 ? 0.0f : 1.0f;
    }
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct WarpedMesh {
    std::array<SDL_Vertex, 4> vertices{};
    std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
    bool valid = false;
};

constexpr float kQuadEpsilon = 1e-5f;

float cross(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool on_segment(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& p) {
    return p.x >= std::min(a.x, b.x) - kQuadEpsilon &&
           p.x <= std::max(a.x, b.x) + kQuadEpsilon &&
           p.y >= std::min(a.y, b.y) - kQuadEpsilon &&
           p.y <= std::max(a.y, b.y) + kQuadEpsilon;
}

bool segments_intersect(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& c, const SDL_FPoint& d) {
    const float d1 = cross(a, b, c);
    const float d2 = cross(a, b, d);
    const float d3 = cross(c, d, a);
    const float d4 = cross(c, d, b);

    const bool d1_zero = std::abs(d1) <= kQuadEpsilon;
    const bool d2_zero = std::abs(d2) <= kQuadEpsilon;
    const bool d3_zero = std::abs(d3) <= kQuadEpsilon;
    const bool d4_zero = std::abs(d4) <= kQuadEpsilon;

    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
        ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f))) {
        return true;
    }
    if (d1_zero && on_segment(a, b, c)) return true;
    if (d2_zero && on_segment(a, b, d)) return true;
    if (d3_zero && on_segment(c, d, a)) return true;
    if (d4_zero && on_segment(c, d, b)) return true;
    return false;
}

bool is_convex_quad(const std::array<SDL_FPoint, 4>& points) {
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const SDL_FPoint& a = points[i];
        const SDL_FPoint& b = points[(i + 1) % 4];
        const SDL_FPoint& c = points[(i + 2) % 4];
        const float area = cross(a, b, c);
        if (std::abs(area) <= kQuadEpsilon) {
            continue;
        }
        if (sign == 0.0f) {
            sign = area;
        } else if ((sign > 0.0f) != (area > 0.0f)) {
            return false;
        }
    }
    return true;
}

static bool enforce_trapezoid(std::array<SDL_FPoint, 4>& points) {
    const bool intersects = segments_intersect(points[0], points[1], points[2], points[3]) ||
        segments_intersect(points[1], points[2], points[3], points[0]);
    const bool convex = is_convex_quad(points);
    if (!intersects && convex) {
        return false;
    }

    std::array<int, 4> indices{0, 1, 2, 3};
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        if (points[a].y != points[b].y) {
            return points[a].y < points[b].y;
        }
        return points[a].x < points[b].x;
    });

    int top_left = indices[0];
    int top_right = indices[1];
    int bottom_left = indices[2];
    int bottom_right = indices[3];

    if (points[top_left].x > points[top_right].x) {
        std::swap(top_left, top_right);
    }
    if (points[bottom_left].x > points[bottom_right].x) {
        std::swap(bottom_left, bottom_right);
    }

    const std::array<SDL_FPoint, 4> reordered{
        points[top_left],
        points[top_right],
        points[bottom_right],
        points[bottom_left]
    };
    points = reordered;
    return true;
}

bool project_world_point(const WarpedScreenGrid& cam,
                         float world_x,
                         float world_y,
                         float world_z,
                         SDL_FPoint& out) {
    if (!cam.project_world_point(SDL_FPoint{world_x, world_y}, world_z, out)) {
        return false;
    }
    return std::isfinite(out.x) && std::isfinite(out.y);
}

bool is_debug_marker_in_bounds(const SDL_FPoint& point, int screen_width, int screen_height) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return false;
    }
    constexpr float kMargin = 16.0f;
    return point.x >= -kMargin &&
           point.y >= -kMargin &&
           point.x <= static_cast<float>(screen_width) + kMargin &&
           point.y <= static_cast<float>(screen_height) + kMargin;
}

anchor_points::AnchorWorldPoint3 debug_child_anchor_world_displacement(const AnchorPoint& parent_anchor,
                                                                       const Asset* child_asset) {
    anchor_points::AnchorWorldPoint3 displacement{};
    if (!child_asset) {
        return displacement;
    }
    const Asset::CumulativeMovementDisplacement cumulative =
        child_asset->current_frame_cumulative_movement_displacement();
    if (!cumulative.valid) {
        return displacement;
    }

    float displacement_x = cumulative.dx;
    float displacement_y = cumulative.dy;
    const float displacement_z = cumulative.dz;
    if (parent_anchor.flip_horizontal) {
        displacement_x = -displacement_x;
    }
    if (parent_anchor.flip_vertical) {
        displacement_y = -displacement_y;
    }

    displacement = anchor_points::AnchorWorldPoint3{
        displacement_x,
        displacement_y,
        displacement_z,
        std::isfinite(displacement_x) &&
            std::isfinite(displacement_y) &&
            std::isfinite(displacement_z)};
    return displacement;
}

void draw_filled_debug_dot(SDL_Renderer* renderer,
                           const SDL_FPoint& center,
                           int radius_px,
                           const SDL_Color& color) {
    if (!renderer || radius_px <= 0) {
        return;
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int cx = static_cast<int>(std::lround(center.x));
    const int cy = static_cast<int>(std::lround(center.y));
    for (int dy = -radius_px; dy <= radius_px; ++dy) {
        const int inside = radius_px * radius_px - dy * dy;
        if (inside < 0) {
            continue;
        }
        const int dx = static_cast<int>(std::floor(std::sqrt(static_cast<float>(inside))));
        SDL_RenderLine(renderer,
                       static_cast<float>(cx - dx),
                       static_cast<float>(cy + dy),
                       static_cast<float>(cx + dx),
                       static_cast<float>(cy + dy));
    }
}

void render_anchor_debug_markers(SDL_Renderer* renderer,
                                 const WarpedScreenGrid& cam,
                                 int screen_width,
                                 int screen_height,
                                 const std::vector<Asset*>& assets,
                                 bool dev_mode) {
    if (!renderer || assets.empty()) {
        return;
    }

    static std::uint64_t s_anchor_debug_frame_counter = 0;
    ++s_anchor_debug_frame_counter;
    const bool emit_parity_logs = (s_anchor_debug_frame_counter % 30u) == 0u;

    const SDL_Color kFlatColor{255, 32, 32, 220};
    const SDL_Color kFinalColor{48, 128, 255, 255};
    const SDL_Color kAnchorParityColor{255, 224, 64, 255};
    const SDL_Color kChildParityColor{64, 255, 160, 255};
    const SDL_Color kParityLineColor{255, 224, 64, 190};
    constexpr int kFlatRadiusPx = 5;
    constexpr int kFinalRadiusPx = 3;
    constexpr int kAnchorParityRadiusPx = 4;
    constexpr int kChildParityRadiusPx = 4;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (Asset* asset : assets) {
        if (!asset || asset->dead || !asset->current_frame) {
            continue;
        }

        for (const DisplacedAssetAnchorPoint& authored_anchor : asset->current_frame->anchor_points) {
            if (!authored_anchor.is_valid()) {
                continue;
            }

            anchor_points::FrameAnchorSample sample{};
            try {
                sample = anchor_points::resolve_frame_anchor_sample(
                    *asset,
                    authored_anchor,
                    anchor_points::GridMaterialization::None);
            } catch (const std::exception&) {
                continue;
            }

            if (sample.resolved.missing) {
                continue;
            }

            if (sample.has_flat_screen_px &&
                is_debug_marker_in_bounds(sample.flat_screen_px, screen_width, screen_height)) {
                draw_filled_debug_dot(renderer, sample.flat_screen_px, kFlatRadiusPx, kFlatColor);
            }

            if (sample.has_final_screen_px &&
                is_debug_marker_in_bounds(sample.final_screen_px, screen_width, screen_height)) {
                draw_filled_debug_dot(renderer, sample.final_screen_px, kFinalRadiusPx, kFinalColor);
            }
        }
    }

    const auto bindings = anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().debug_bindings_snapshot();
    for (const auto& binding : bindings) {
        Asset* owner = binding.owner;
        Asset* child = binding.child_asset;
        if (!owner || !child || owner->dead || child->dead || binding.anchor_name.empty()) {
            continue;
        }

        const auto anchor = owner->anchor_state(binding.anchor_name,
                                                anchor_points::GridMaterialization::None,
                                                Asset::AnchorResolveMode::ForceRecompute);
        if (!anchor.has_value() || !anchor->is_active()) {
            continue;
        }

        anchored_child_placement::PlacementInput expected_input{};
        expected_input.parent.world_x = static_cast<float>(owner->world_x());
        expected_input.parent.world_y = static_cast<float>(owner->world_y());
        expected_input.parent.world_z = static_cast<float>(owner->world_z());
        expected_input.parent.resolution_layer =
            owner->grid_point() ? owner->grid_point()->resolution_layer() : owner->grid_resolution;
        expected_input.anchor_definition.anchor = *anchor;
        const anchor_points::AnchorWorldPoint3 anchor_displacement =
            debug_child_anchor_world_displacement(*anchor, child);
        if (anchor_displacement.valid) {
            expected_input.anchor_world_displacement.x = anchor_displacement.x;
            expected_input.anchor_world_displacement.y = anchor_displacement.y;
            expected_input.anchor_world_displacement.z = anchor_displacement.z;
        }
        expected_input.sprite_transform.mirror_x = anchor->flip_horizontal;
        expected_input.sprite_transform.mirror_y = anchor->flip_vertical;
        expected_input.sprite_transform.rotation_degrees = anchor->rotation_degrees;
        expected_input.camera_state.camera = &cam;

        anchored_child_placement::PlacementOutput expected{};
        if (!anchored_child_placement::resolve_child_placement(expected_input, expected) ||
            !expected.has_child_screen_px) {
            continue;
        }

        SDL_FPoint actual_child_screen{};
        const bool have_actual_child = anchored_child_placement::project_child_pivot_screen(
            cam,
            child->smoothed_translation_x() + child->render_anchor_offset_x(),
            child->smoothed_translation_y() + child->render_anchor_offset_y(),
            static_cast<float>(child->world_z()) + child->world_z_offset() + child->render_anchor_offset_z(),
            actual_child_screen);
        if (!have_actual_child) {
            continue;
        }

        if (is_debug_marker_in_bounds(expected.child_screen_px, screen_width, screen_height)) {
            draw_filled_debug_dot(renderer, expected.child_screen_px, kAnchorParityRadiusPx, kAnchorParityColor);
        }
        if (is_debug_marker_in_bounds(actual_child_screen, screen_width, screen_height)) {
            draw_filled_debug_dot(renderer, actual_child_screen, kChildParityRadiusPx, kChildParityColor);
        }
        if (is_debug_marker_in_bounds(expected.child_screen_px, screen_width, screen_height) ||
            is_debug_marker_in_bounds(actual_child_screen, screen_width, screen_height)) {
            SDL_SetRenderDrawColor(renderer,
                                   kParityLineColor.r,
                                   kParityLineColor.g,
                                   kParityLineColor.b,
                                   kParityLineColor.a);
            SDL_RenderLine(renderer,
                           expected.child_screen_px.x,
                           expected.child_screen_px.y,
                           actual_child_screen.x,
                           actual_child_screen.y);
        }

        if (!emit_parity_logs) {
            continue;
        }

        const float dx = actual_child_screen.x - expected.child_screen_px.x;
        const float dy = actual_child_screen.y - expected.child_screen_px.y;
        const float delta_px = std::sqrt(dx * dx + dy * dy);
        constexpr float kParityTolerancePx = 0.5f;
        if (delta_px <= kParityTolerancePx) {
            continue;
        }

        const std::string mode_label = dev_mode ? "dev" : "normal";
        const std::string message = std::string("[AnchorParity][") + mode_label + "] owner='" +
                                    (owner->info ? owner->info->name : std::string{"<unknown>"}) +
                                    "' child='" +
                                    (child->info ? child->info->name : std::string{"<unknown>"}) +
                                    "' anchor='" + binding.anchor_name +
                                    "' delta_px=" + std::to_string(delta_px);
        vibble::log::warn(message);
    }
}

bool project_floor_debug_point(const WarpedScreenGrid& cam, SDL_Point world_xz, SDL_FPoint& out) {
    SDL_FPoint projected{};
    if (!cam.project_world_point(SDL_FPoint{static_cast<float>(world_xz.x), 0.0f},
                                 static_cast<float>(world_xz.y),
                                 projected)) {
        return false;
    }
    if (!std::isfinite(projected.x) || !std::isfinite(projected.y)) {
        return false;
    }
    projected.y = cam.warp_floor_screen_y(0.0f, projected.y);
    if (!std::isfinite(projected.y)) {
        return false;
    }
    out = projected;
    return true;
}

std::uint32_t stable_path_color_hash(const std::string& animation_id, std::size_t path_index) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : animation_id) {
        hash ^= c;
        hash *= 16777619u;
    }
    hash ^= static_cast<std::uint32_t>(path_index & 0xffffffffu);
    hash *= 16777619u;
    return hash;
}

SDL_Color stable_movement_path_color(const std::string& animation_id, std::size_t path_index) {
    constexpr std::array<SDL_Color, 12> kPalette{{
        SDL_Color{48, 200, 255, 220},
        SDL_Color{255, 214, 64, 220},
        SDL_Color{120, 235, 110, 220},
        SDL_Color{255, 128, 96, 220},
        SDL_Color{130, 170, 255, 220},
        SDL_Color{255, 156, 220, 220},
        SDL_Color{96, 240, 210, 220},
        SDL_Color{240, 178, 96, 220},
        SDL_Color{190, 240, 112, 220},
        SDL_Color{255, 96, 150, 220},
        SDL_Color{140, 210, 255, 220},
        SDL_Color{255, 196, 120, 220},
    }};
    const std::uint32_t hash = stable_path_color_hash(animation_id, path_index);
    return kPalette[hash % kPalette.size()];
}

bool build_perspective_mesh(RenderObject& obj,
                            const WarpedScreenGrid& cam,
                            float perspective_scale,
                            float base_world_z,
                            WarpedMesh& mesh) {
    if (!obj.texture) {
        return false;
    }

    const SDL_Rect& rect = obj.screen_rect;
    if (rect.w <= 0 || rect.h <= 0) {
        return false;
    }
    const float anchor_world_x =
        std::isfinite(obj.world_anchor_x) ? obj.world_anchor_x : static_cast<float>(rect.x);
    const float anchor_world_y =
        std::isfinite(obj.world_anchor_y) ? obj.world_anchor_y : static_cast<float>(rect.y);
    const SDL_FPoint current_position{anchor_world_x, anchor_world_y};

    // Render package dimensions are perspective-inclusive; pass them through unchanged
    // so projected frame construction uses a single consistent contract.
    const float safe_perspective = render_projection::sanitize_perspective_scale(perspective_scale);
    const float current_scale = safe_perspective;
    const std::uint64_t current_camera_version = cam.camera_state_version();
    auto quantize_projection_key = [](float value) -> std::int64_t {
        if (!std::isfinite(value)) {
            return std::numeric_limits<std::int64_t>::min();
        }
        constexpr double kFixedPointScale = 256.0;
        return static_cast<std::int64_t>(std::llround(static_cast<double>(value) * kFixedPointScale));
    };
    const std::int64_t current_position_key_x = quantize_projection_key(current_position.x);
    const std::int64_t current_position_key_y = quantize_projection_key(current_position.y);
    const std::int64_t current_world_z_key = quantize_projection_key(base_world_z);
    const std::int64_t current_scale_key = quantize_projection_key(current_scale);
    if (!obj.mesh_dirty &&
        obj.has_cached_mesh &&
        obj.cached_mesh_texture == obj.texture &&
        obj.cached_projection_key_valid &&
        obj.cached_position_key_x == current_position_key_x &&
        obj.cached_position_key_y == current_position_key_y &&
        obj.cached_world_z_key == current_world_z_key &&
        obj.cached_scale_key == current_scale_key &&
        obj.cached_camera_state_version == current_camera_version) {
        mesh.vertices = obj.cached_vertices;
        mesh.indices = obj.cached_indices;
        mesh.valid = true;
        return true;
    }
    render_projection::SpriteProjectionInput projection_input{};
    if (!render_projection::assemble_render_object_projection_input(
            obj, safe_perspective, base_world_z, projection_input)) {
        return false;
    }

    if (obj.dimension_cache_texture != obj.texture) {
        obj.dimension_cache_texture = obj.texture;
        obj.has_atlas_size = false;
        obj.has_texture_size = false;
        obj.has_cached_mesh = false;
    }

    if (!obj.has_atlas_size) {
        float atlas_wf = 0.0f;
        float atlas_hf = 0.0f;
        if (!SDL_GetTextureSize(obj.texture, &atlas_wf, &atlas_hf)) {
            return false;
        }
        obj.atlas_w = static_cast<int>(std::lround(atlas_wf));
        obj.atlas_h = static_cast<int>(std::lround(atlas_hf));
        obj.has_atlas_size = true;
        if (!obj.has_texture_size) {
            obj.texture_w = obj.atlas_w;
            obj.texture_h = obj.atlas_h;
            obj.has_texture_size = (obj.texture_w > 0 && obj.texture_h > 0);
        }
    }

    const int atlas_w = obj.atlas_w;
    const int atlas_h = obj.atlas_h;
    if (atlas_w <= 0 || atlas_h <= 0) {
        return false;
    }

    int tex_w = obj.texture_w;
    int tex_h = obj.texture_h;
    if (!obj.has_texture_size) {
        tex_w = atlas_w;
        tex_h = atlas_h;
        obj.texture_w = tex_w;
        obj.texture_h = tex_h;
        obj.has_texture_size = (tex_w > 0 && tex_h > 0);
    }
    if (tex_w <= 0 || tex_h <= 0) {
        return false;
    }

    float u0 = 0.0f;
    float u1 = 1.0f;
    float v0 = 0.0f;
    float v1 = 1.0f;

    if (obj.has_src_rect) {
        const SDL_Rect& r = obj.src_rect;
        const float pad_x = 0.5f / static_cast<float>(atlas_w);
        const float pad_y = 0.5f / static_cast<float>(atlas_h);
        u0 = (static_cast<float>(r.x) + pad_x) / static_cast<float>(atlas_w);
        u1 = (static_cast<float>(r.x + r.w) - pad_x) / static_cast<float>(atlas_w);
        v0 = (static_cast<float>(r.y) + pad_y) / static_cast<float>(atlas_h);
        v1 = (static_cast<float>(r.y + r.h) - pad_y) / static_cast<float>(atlas_h);
        tex_w = r.w;
        tex_h = r.h;
    } else {
        const float padding_x = 0.5f / static_cast<float>(tex_w);
        const float padding_y = 0.5f / static_cast<float>(tex_h);
        u0 = padding_x;
        u1 = 1.0f - padding_x;
        v0 = padding_y;
        v1 = 1.0f - padding_y;
    }

    if ((obj.flip & SDL_FLIP_HORIZONTAL) != 0) {
        std::swap(u0, u1);
    }
    if ((obj.flip & SDL_FLIP_VERTICAL) != 0) {
        std::swap(v0, v1);
    }

    render_projection::ProjectedSpriteFrame projection{};
    if (!render_projection::build_projected_sprite_frame(cam, projection_input, projection)) {
        return false;
    }

    const SDL_FColor vertex_color{
        obj.color_mod.r / 255.0f,
        obj.color_mod.g / 255.0f,
        obj.color_mod.b / 255.0f,
        obj.color_mod.a / 255.0f};

    SDL_Vertex vtx_tl{};
    vtx_tl.position = projection.screen_tl;
    vtx_tl.color = vertex_color;
    vtx_tl.tex_coord = SDL_FPoint{u0, v0};

    SDL_Vertex vtx_tr{};
    vtx_tr.position = projection.screen_tr;
    vtx_tr.color = vertex_color;
    vtx_tr.tex_coord = SDL_FPoint{u1, v0};

    SDL_Vertex vtx_br{};
    vtx_br.position = projection.screen_br;
    vtx_br.color = vertex_color;
    vtx_br.tex_coord = SDL_FPoint{u1, v1};

    SDL_Vertex vtx_bl{};
    vtx_bl.position = projection.screen_bl;
    vtx_bl.color = vertex_color;
    vtx_bl.tex_coord = SDL_FPoint{u0, v1};

    mesh.vertices = {vtx_tl, vtx_tr, vtx_br, vtx_bl};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.valid = true;
    obj.cached_vertices = mesh.vertices;
    obj.cached_indices = mesh.indices;
    obj.cached_position = current_position;
    obj.cached_world_z = base_world_z;
    obj.cached_scale = current_scale;
    obj.cached_position_key_x = current_position_key_x;
    obj.cached_position_key_y = current_position_key_y;
    obj.cached_world_z_key = current_world_z_key;
    obj.cached_scale_key = current_scale_key;
    obj.cached_projection_key_valid = true;
    obj.cached_camera_state_version = current_camera_version;
    obj.cached_mesh_texture = obj.texture;
    obj.has_cached_mesh = true;
    obj.mesh_dirty = false;

    return true;
}

}

SceneRenderer::SceneRenderer(SDL_Renderer* renderer,
                             Assets* assets,
                             int screen_width,
                             int screen_height,
                             const nlohmann::json& map_manifest,
                             const std::string& map_id)
: SceneRenderer(require_prerequisites(renderer, assets),
                renderer,
                assets,
                screen_width,
                screen_height,
                map_manifest,
                map_id) {}

SceneRenderer::PrevalidatedTag SceneRenderer::require_prerequisites(SDL_Renderer* renderer, Assets* assets) {
    std::string reason;
    if (!SceneRenderer::prerequisites_ready(renderer, assets, &reason)) {
        const std::string message = reason.empty() ? "SceneRenderer prerequisites missing." : reason;
        vibble::log::error(std::string{"[SceneRenderer] Initialization aborted: "} + message);
        if (!renderer) { SDL_assert(renderer != nullptr); }
        if (!assets)   { SDL_assert(assets != nullptr); }
        throw std::invalid_argument(message);
    }
    return PrevalidatedTag{};
}

SceneRenderer::SceneRenderer(PrevalidatedTag,
                             SDL_Renderer* renderer,
                             Assets* assets,
                             int screen_width,
                             int screen_height,
                             const nlohmann::json& map_manifest,
                             const std::string& map_id)
: renderer_(renderer),
  assets_(assets),
  screen_width_(screen_width),
  screen_height_(screen_height),
  tile_renderer_(std::make_unique<GridTileRenderer>(assets)),
  geometry_batcher_(std::make_unique<GeometryBatcher>(renderer)),
  sky_texture_path_(std::filesystem::path("resources") / "misc_content" / "sky.png"),
  composite_renderer_(renderer, assets),
  layer_effect_processor_(renderer),
  dynamic_boundary_system_(std::make_unique<DynamicBoundarySystem>())
{

    map_clear_color_ = SDL_Color{69, 101, 74, 255};

    // Initialize dynamic boundary system
    if (dynamic_boundary_system_) {
        if (!dynamic_boundary_system_->initialize(renderer_, &assets_->library())) {
            vibble::log::warn("[SceneRenderer] Failed to initialize dynamic boundary system");
        }
    }

    {
        auto read_float_from = [](const nlohmann::json& obj, const char* key, float& target) {
            auto it = obj.find(key);
            if (it == obj.end()) {
                return;
            }
            if (it->is_number_float()) {
                target = static_cast<float>(it->get<double>());
            } else if (it->is_number_integer()) {
                target = static_cast<float>(it->get<int>());
            }
        };
        auto read_bool_from = [](const nlohmann::json& obj, const char* key, bool& target) {
            auto it = obj.find(key);
            if (it != obj.end() && it->is_boolean()) {
                target = it->get<bool>();
            }
        };
        auto parse_vec2 = [](const nlohmann::json& obj, const char* key, SDL_FPoint current) -> SDL_FPoint {
            auto it = obj.find(key);
            if (it == obj.end()) {
                return current;
            }
            SDL_FPoint out = current;
            if (it->is_array() && it->size() >= 2) {
                if ((*it)[0].is_number()) { out.x = static_cast<float>((*it)[0].get<double>()); }
                if ((*it)[1].is_number()) { out.y = static_cast<float>((*it)[1].get<double>()); }
                return out;
            }
            if (it->is_object()) {
                auto x_it = it->find("x");
                auto y_it = it->find("y");
                if (x_it != it->end() && x_it->is_number()) { out.x = static_cast<float>(x_it->get<double>()); }
                if (y_it != it->end() && y_it->is_number()) { out.y = static_cast<float>(y_it->get<double>()); }
                return out;
            }
            return current;
        };

        // TODO: terrain/light settings parsing temporarily disabled during axis refactor.
    }

    map_radius_world_ = map_layers::map_radius_from_map_info(map_manifest);
    if (!std::isfinite(map_radius_world_) || map_radius_world_ <= 0.0) {
        map_radius_world_ = static_cast<double>(std::max(screen_width_, screen_height_));
    }

    vibble::log::debug(std::string{"[SceneRenderer] Initializing for map '"} + map_id +
                       "' with screen " + std::to_string(screen_width_) + "x" + std::to_string(screen_height_) + ".");

    std::cout<<"[SceneRenderer] Init complete."<<std::endl;
}

SceneRenderer::~SceneRenderer() {
    destroy_sky_texture();
    if (scene_composite_tex_) { SDL_DestroyTexture(scene_composite_tex_); scene_composite_tex_ = nullptr; }
    if (postprocess_tex_)     { SDL_DestroyTexture(postprocess_tex_);     postprocess_tex_     = nullptr; }
    if (blur_tex_)            { SDL_DestroyTexture(blur_tex_);            blur_tex_            = nullptr; }
    for (SDL_Texture* tex : motion_blur_history_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    motion_blur_history_capacity_ = 0;
    for (SDL_Texture* tex : dof_layer_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (SDL_Texture* tex : dof_dark_mask_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (SDL_Texture* tex : dof_lit_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (SDL_Texture* tex : dof_blur_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    motion_blur_history_textures_.clear();
    motion_blur_history_write_index_ = 0;
    motion_blur_valid_history_frames_ = 0;
    motion_blur_history_capacity_ = 0;
    dof_layer_textures_.clear();
    dof_dark_mask_textures_.clear();
    dof_lit_textures_.clear();
    dof_blur_textures_.clear();
}

bool SceneRenderer::ensure_sky_texture() {
    if (sky_texture_) {
        return true;
    }
    if (sky_texture_failed_ || !renderer_) {
        return false;
    }

    const std::filesystem::path path = sky_texture_path_;
    if (path.empty() || !std::filesystem::exists(path)) {
        vibble::log::warn(std::string{"[SceneRenderer] Sky texture missing at "} + path.string());
        sky_texture_failed_ = true;
        return false;
    }

    SDL_Texture* tex = IMG_LoadTexture(renderer_, path.string().c_str());
    if (!tex) {
        vibble::log::warn(std::string{"[SceneRenderer] Failed to load sky texture: "} + SDL_GetError());
        sky_texture_failed_ = true;
        return false;
    }

    float tex_w = 0.0f;
    float tex_h = 0.0f;
    if (!SDL_GetTextureSize(tex, &tex_w, &tex_h) || tex_w <= 0.0f || tex_h <= 0.0f) {
        vibble::log::warn("[SceneRenderer] Sky texture has invalid dimensions.");
        SDL_DestroyTexture(tex);
        sky_texture_failed_ = true;
        return false;
    }

    WarpedScreenGrid::RealismSettings realism{};
    if (assets_) {
        realism = assets_->getView().get_settings();
    }
    const double f_stop = std::max(0.01, static_cast<double>(realism.aperture_f_stop));
    const double focal_length = std::max(0.01, static_cast<double>(realism.focal_length_mm));
    const double max_cull_depth = std::max(1.0, static_cast<double>(realism.max_cull_depth));
    const double max_blur = std::max(0.0, static_cast<double>(realism.max_blur_px));
    const bool depth_of_field_enabled = realism.depth_of_field_enabled;
    const bool background_layer_fog_enabled = kBackgroundLayerFogEnabled;
    const double sky_blur_radius = LayerEffectProcessor::coc_blur_radius_from_depth_delta(max_cull_depth,
                                                                                            max_cull_depth,
                                                                                            focal_length,
                                                                                            f_stop,
                                                                                            max_blur);
    if (sky_blur_radius > 0.01 && depth_of_field_enabled) {
        SDL_Texture* blurred_sky = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            static_cast<int>(std::lround(tex_w)), static_cast<int>(std::lround(tex_h)));
        if (blurred_sky) {
            SDL_Texture* sky_scratch = SDL_CreateTexture(
                renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                static_cast<int>(std::lround(tex_w)), static_cast<int>(std::lround(tex_h)));
            SDL_SetTextureBlendMode(blurred_sky, SDL_BLENDMODE_BLEND);
            if (sky_scratch) {
                SDL_SetTextureBlendMode(sky_scratch, SDL_BLENDMODE_BLEND);
                const double radial_factor = LayerEffectProcessor::radial_lens_factor_from_optics(focal_length, f_stop);
                const double radial_radius = std::clamp(sky_blur_radius * radial_factor, 0.0, max_blur * 2.0);
                const SDL_FPoint sky_optical_center{tex_w * 0.5f, tex_h * 0.5f};
                if (layer_effect_processor_.apply_lens_blur(tex,
                                                            blurred_sky,
                                                            sky_scratch,
                                                            static_cast<int>(std::lround(tex_w)),
                                                            static_cast<int>(std::lround(tex_h)),
                                                            static_cast<float>(sky_blur_radius),
                                                            sky_optical_center,
                                                            static_cast<float>(radial_radius),
                                                            0.55f)) {
                    SDL_DestroyTexture(tex);
                    tex = blurred_sky;
                } else {
                    SDL_DestroyTexture(blurred_sky);
                }
                SDL_DestroyTexture(sky_scratch);
            } else {
                vibble::log::warn(std::string{"[SceneRenderer] Failed to allocate sky blur scratch texture: "} + SDL_GetError());
                SDL_DestroyTexture(blurred_sky);
            }
        } else {
            vibble::log::warn(std::string{"[SceneRenderer] Failed to allocate blurred sky target: "} + SDL_GetError());
        }
    }

    sky_texture_ = tex;
    sky_texture_width_ = static_cast<int>(std::lround(tex_w));
    sky_texture_height_ = static_cast<int>(std::lround(tex_h));
    sky_texture_failed_ = false;
    return true;
}

void SceneRenderer::destroy_sky_texture() {
    if (sky_texture_) {
        SDL_DestroyTexture(sky_texture_);
        sky_texture_ = nullptr;
    }
    sky_texture_width_ = 0;
    sky_texture_height_ = 0;
    sky_texture_failed_ = false;
}

SDL_Renderer* SceneRenderer::get_renderer() const {
    return renderer_;
}

void SceneRenderer::set_output_dimensions(int screen_width, int screen_height) {
    const int safe_w = std::max(1, screen_width);
    const int safe_h = std::max(1, screen_height);
    if (safe_w == screen_width_ && safe_h == screen_height_) {
        return;
    }

    screen_width_ = safe_w;
    screen_height_ = safe_h;
    map_radius_world_ = static_cast<double>(std::max(screen_width_, screen_height_));

    if (scene_composite_tex_) {
        SDL_DestroyTexture(scene_composite_tex_);
        scene_composite_tex_ = nullptr;
    }
    if (postprocess_tex_) {
        SDL_DestroyTexture(postprocess_tex_);
        postprocess_tex_ = nullptr;
    }
    if (blur_tex_) {
        SDL_DestroyTexture(blur_tex_);
        blur_tex_ = nullptr;
    }
    for (SDL_Texture* tex : motion_blur_history_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (SDL_Texture* tex : dof_layer_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (SDL_Texture* tex : dof_dark_mask_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (SDL_Texture* tex : dof_lit_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (SDL_Texture* tex : dof_blur_textures_) {
        if (tex) SDL_DestroyTexture(tex);
    }
    motion_blur_history_textures_.clear();
    motion_blur_history_write_index_ = 0;
    motion_blur_valid_history_frames_ = 0;
    dof_layer_textures_.clear();
    dof_dark_mask_textures_.clear();
    dof_lit_textures_.clear();
    dof_blur_textures_.clear();
}

std::optional<SDL_Point> SceneRenderer::postprocess_target_size() const {
    SDL_Texture* target = postprocess_tex_ ? postprocess_tex_
                                           : (scene_composite_tex_ ? scene_composite_tex_ : blur_tex_);
    if (!target) {
        return std::nullopt;
    }
    float w = 0.0f;
    float h = 0.0f;
    if (!SDL_GetTextureSize(target, &w, &h) || w <= 0.0f || h <= 0.0f) {
        return std::nullopt;
    }
    return SDL_Point{static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))};
}

void SceneRenderer::set_movement_debug_enabled(bool enabled) {
    if (debug_auto_paths_ == enabled) {
        return;
    }
    debug_auto_paths_ = enabled;
    if (!enabled) {
        movement_debug_snapshots_.clear();
        movement_debug_observed_state_.clear();
    }
}

void SceneRenderer::set_movement_debug_visible(bool visible) {
    movement_debug_visible_ = visible;
}

void SceneRenderer::set_anchor_point_debug_enabled(bool enabled) {
    anchor_point_debug_enabled_ = enabled;
}

void SceneRenderer::invalidate_dynamic_boundary_system() {
    if (dynamic_boundary_system_) {
        dynamic_boundary_system_->invalidate_config();
    }
}

const std::vector<DynamicBoundarySystem::BoundarySprite>& SceneRenderer::dynamic_boundary_sprites() const {
    static const std::vector<DynamicBoundarySystem::BoundarySprite> kEmptyBoundarySprites;
    if (!dynamic_boundary_system_) {
        return kEmptyBoundarySprites;
    }
    return dynamic_boundary_system_->get_boundary_sprites();
}

void SceneRenderer::gather_runtime_lights(const WarpedScreenGrid& cam,
                                          const std::vector<Asset*>& rendered_assets,
                                          std::vector<LayerEffectProcessor::RuntimeLight>& out_lights) const {
    out_lights.clear();
    if (!assets_) {
        return;
    }
    out_lights.reserve(rendered_assets.size());

    constexpr float kCullingMargin = 128.0f;
    for (Asset* asset : rendered_assets) {
        if (!asset || asset->dead || !asset->current_frame) {
            continue;
        }
        for (const DisplacedAssetAnchorPoint& anchor : asset->current_frame->anchor_points) {
            if (!anchor.is_valid() || !anchor.has_light_data || !anchor.light.enabled) {
                continue;
            }

            const std::optional<AnchorPoint> resolved = asset->anchor_state(
                anchor.name,
                anchor_points::GridMaterialization::None,
                Asset::AnchorResolveMode::Cached);
            if (!resolved.has_value() || !resolved->exists) {
                continue;
            }

            SDL_FPoint screen{};
            if (!cam.project_world_point(SDL_FPoint{resolved->world_exact_pos_2d.x, resolved->world_exact_pos_2d.y},
                                         resolved->world_exact_z,
                                         screen) ||
                !std::isfinite(screen.x) ||
                !std::isfinite(screen.y)) {
                continue;
            }

            AnchorLightData light = anchor.light;
            light.sanitize();
            const float perspective_scale = resolved->has_flat_perspective_scale
                ? std::max(0.05f, resolved->flat_perspective_scale)
                : 1.0f;
            const float radius_px = std::max(4.0f, light.radius * perspective_scale);
            if (screen.x + radius_px < -kCullingMargin ||
                screen.x - radius_px > static_cast<float>(screen_width_) + kCullingMargin ||
                screen.y + radius_px < -kCullingMargin ||
                screen.y - radius_px > static_cast<float>(screen_height_) + kCullingMargin) {
                continue;
            }

            LayerEffectProcessor::RuntimeLight instance{};
            instance.screen_center = screen;
            instance.color = SDL_Color{light.color_r, light.color_g, light.color_b, 255};
            instance.intensity = light.intensity;
            instance.radius_px = radius_px;
            instance.falloff = light.falloff;
            instance.world_z = static_cast<float>(
                render_depth::depth_from_anchor(cam.anchor_world_z(),
                                                static_cast<double>(resolved->world_exact_z)));
            out_lights.push_back(instance);
        }
    }
}

void SceneRenderer::refresh_movement_debug_snapshots(const std::vector<Asset*>& visible_assets) {
    std::unordered_set<const Asset*> visible_set;
    visible_set.reserve(visible_assets.size());
    for (const Asset* asset : visible_assets) {
        if (asset && !asset->dead) {
            visible_set.insert(asset);
        }
    }

    for (auto it = movement_debug_snapshots_.begin(); it != movement_debug_snapshots_.end();) {
        if (visible_set.find(it->first) == visible_set.end()) {
            it = movement_debug_snapshots_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = movement_debug_observed_state_.begin(); it != movement_debug_observed_state_.end();) {
        if (visible_set.find(it->first) == visible_set.end()) {
            it = movement_debug_observed_state_.erase(it);
        } else {
            ++it;
        }
    }

    for (Asset* asset : visible_assets) {
        if (!asset || asset->dead || !asset->info || !asset->current_frame) {
            continue;
        }

        const std::string current_animation = asset->current_animation;
        const AnimationFrame* current_frame = asset->current_frame;
        const bool current_is_first = current_frame->is_first;
        const bool current_is_last = current_frame->is_last;

        const auto observed_it = movement_debug_observed_state_.find(asset);
        const bool has_previous_state = observed_it != movement_debug_observed_state_.end();
        const bool has_snapshot = movement_debug_snapshots_.find(asset) != movement_debug_snapshots_.end();

        bool loop_trigger = false;
        bool end_trigger = false;
        if (has_previous_state) {
            const MovementDebugObservedState& previous = observed_it->second;
            loop_trigger = previous.frame_is_last &&
                           previous.animation_id == current_animation &&
                           current_is_first;
            end_trigger = previous.frame_is_last &&
                          previous.animation_id != current_animation;
        }

        const bool initial_trigger = !has_snapshot;
        const bool should_refresh = initial_trigger || loop_trigger || end_trigger;
        if (should_refresh) {
            MovementDebugAssetSnapshot snapshot{};
            const SDL_Point origin = asset->world_xz_point();

            for (const auto& [animation_id, animation] : asset->info->animations) {
                const std::size_t path_count = animation.movement_path_count();
                for (std::size_t path_index = 0; path_index < path_count; ++path_index) {
                    const auto& movement_path = animation.movement_path(path_index);
                    if (movement_path.empty()) {
                        continue;
                    }

                    MovementDebugPathSnapshot path_snapshot{};
                    path_snapshot.color = stable_movement_path_color(animation_id, path_index);
                    path_snapshot.world_points.reserve(movement_path.size() + 1);
                    path_snapshot.world_points.push_back(origin);

                    int world_x = origin.x;
                    int world_z = origin.y;
                    bool has_movement = false;

                    for (const AnimationFrame& frame : movement_path) {
                        if (frame.dx != 0 || frame.dz != 0) {
                            has_movement = true;
                        }
                        world_x += frame.dx;
                        world_z += frame.dz;

                        const SDL_Point next_point{world_x, world_z};
                        if (path_snapshot.world_points.empty() ||
                            next_point.x != path_snapshot.world_points.back().x ||
                            next_point.y != path_snapshot.world_points.back().y) {
                            path_snapshot.world_points.push_back(next_point);
                        }
                    }

                    if (!has_movement || path_snapshot.world_points.size() < 2) {
                        continue;
                    }
                    snapshot.paths.push_back(std::move(path_snapshot));
                }
            }

            if (snapshot.paths.empty()) {
                movement_debug_snapshots_.erase(asset);
            } else {
                movement_debug_snapshots_[asset] = std::move(snapshot);
            }
        }

        movement_debug_observed_state_[asset] = MovementDebugObservedState{
            current_animation,
            current_frame,
            current_is_first,
            current_is_last
        };
    }
}

void SceneRenderer::render_movement_debug_snapshots(const WarpedScreenGrid& cam,
                                                    int screen_width,
                                                    int screen_height,
                                                    const std::vector<Asset*>& visible_assets) const {
    if (!renderer_) {
        return;
    }

    constexpr SDL_Color kStartColor{64, 255, 128, 230};
    constexpr int kStartRadius = 3;
    constexpr int kEndRadius = 3;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    for (const Asset* asset : visible_assets) {
        if (!asset || asset->dead) {
            continue;
        }
        const auto snapshot_it = movement_debug_snapshots_.find(asset);
        if (snapshot_it == movement_debug_snapshots_.end()) {
            continue;
        }

        const MovementDebugAssetSnapshot& snapshot = snapshot_it->second;
        for (const MovementDebugPathSnapshot& path : snapshot.paths) {
            if (path.world_points.size() < 2) {
                continue;
            }

            std::vector<SDL_FPoint> projected_points(path.world_points.size(), SDL_FPoint{0.0f, 0.0f});
            std::vector<bool> point_visible(path.world_points.size(), false);
            for (std::size_t i = 0; i < path.world_points.size(); ++i) {
                SDL_FPoint screen{};
                if (project_floor_debug_point(cam, path.world_points[i], screen) &&
                    is_debug_marker_in_bounds(screen, screen_width, screen_height)) {
                    projected_points[i] = screen;
                    point_visible[i] = true;
                }
            }

            SDL_SetRenderDrawColor(renderer_, path.color.r, path.color.g, path.color.b, path.color.a);
            for (std::size_t i = 1; i < projected_points.size(); ++i) {
                if (!point_visible[i - 1] || !point_visible[i]) {
                    continue;
                }
                SDL_RenderLine(renderer_,
                               projected_points[i - 1].x,
                               projected_points[i - 1].y,
                               projected_points[i].x,
                               projected_points[i].y);
            }

            if (point_visible.front()) {
                draw_filled_debug_dot(renderer_, projected_points.front(), kStartRadius, kStartColor);
            }
            if (point_visible.back()) {
                SDL_Color end_color = path.color;
                end_color.a = 240;
                draw_filled_debug_dot(renderer_, projected_points.back(), kEndRadius, end_color);
            }
        }
    }
}

void SceneRenderer::render() {
    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    auto create_target_texture = [&](int w, int h) -> SDL_Texture* {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!texture) {
            return nullptr;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        return texture;
    };

    if (!scene_composite_tex_) {
        scene_composite_tex_ = create_target_texture(screen_width_, screen_height_);
    }

    const bool scene_targets_ready = scene_composite_tex_ != nullptr;
    int available_history_textures = 0;
    if (scene_targets_ready && kSceneMotionBlurEnabled) {
        if (static_cast<int>(motion_blur_history_textures_.size()) != kMotionBlurHistoryFrameCount) {
            for (SDL_Texture* tex : motion_blur_history_textures_) {
                if (tex) {
                    SDL_DestroyTexture(tex);
                }
            }
            motion_blur_history_textures_.assign(kMotionBlurHistoryFrameCount, nullptr);
            motion_blur_history_write_index_ = 0;
            motion_blur_valid_history_frames_ = 0;
        }
        for (int i = 0; i < kMotionBlurHistoryFrameCount; ++i) {
            if (!motion_blur_history_textures_[i]) {
                motion_blur_history_textures_[i] = create_target_texture(screen_width_, screen_height_);
            }
            if (motion_blur_history_textures_[i]) {
                ++available_history_textures;
            }
        }
    }
    motion_blur_history_capacity_ = available_history_textures;

    const bool motion_blur_targets_ready =
        kSceneMotionBlurEnabled && scene_targets_ready && motion_blur_history_capacity_ > 0;
    if (!motion_blur_targets_ready) {
        motion_blur_history_write_index_ = 0;
        motion_blur_valid_history_frames_ = 0;
    }
    motion_blur_valid_history_frames_ =
        std::min(motion_blur_valid_history_frames_, motion_blur_history_capacity_);

    SDL_Texture* const gameplay_target = scene_composite_tex_;
    SDL_SetRenderTarget(renderer_, gameplay_target);
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, map_clear_color_.r, map_clear_color_.g, map_clear_color_.b, map_clear_color_.a);
    SDL_RenderClear(renderer_);
    WarpedScreenGrid& cam = assets_->getView();
    world::WorldGrid& grid = assets_->world_grid();
    render_sky_layer(cam);

    if (!geometry_batcher_) return;

    geometry_batcher_->clear();

    // 1) Ground / tiles
    tile_renderer_->render(renderer_, cam, grid, geometry_batcher_.get());

    // 2) Dynamic boundary decorations
    const bool boundary_assets_visible = assets_->boundary_assets_visible();
    const bool runtime_updates_enabled = assets_->should_run_runtime_updates();
    const bool should_render_boundaries =
        boundary_assets_visible && dynamic_boundary_system_ && dynamic_boundary_system_->is_initialized();
    const float boundary_delta_ms =
        runtime_updates_enabled ? static_cast<float>(assets_->frame_delta_seconds() * 1000.0) : 0.0f;
    if (should_render_boundaries) {
        dynamic_boundary_system_->update(cam, grid, assets_, boundary_delta_ms);
    }

    static const std::vector<DynamicBoundarySystem::BoundarySprite> kEmptyBoundarySprites;
    const auto& boundary_sprites = should_render_boundaries
        ? dynamic_boundary_system_->get_boundary_sprites()
        : kEmptyBoundarySprites;
    // 3) Assets + boundaries (depth-sorted painter's algorithm)
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
    const double anchor_depth = cam.anchor_world_z();
    const WarpedScreenGrid::RealismSettings realism = cam.get_settings();
    const double max_cull_depth = std::max(1.0, static_cast<double>(realism.max_cull_depth));
    const bool depth_of_field_enabled = realism.depth_of_field_enabled;
    const float boundary_vertical_offset = DynamicBoundarySystem::vertical_offset();
    const float boundary_cull_margin = 64.0f;
    const float boundary_min_visible_px =
        static_cast<float>(screen_height_) *
        std::clamp(assets_->boundary_min_visible_screen_ratio(), 0.0f, 0.5f);

    auto queue_boundary_sprite = [&](const DynamicBoundarySystem::BoundarySprite& sprite, double depth_from_anchor) {
        const double depth_distance = std::fabs(depth_from_anchor);
        if (!std::isfinite(depth_distance) || depth_distance > max_cull_depth) {
            return;
        }
        if (!sprite.texture ||
            sprite.texture_w <= 0 || sprite.texture_h <= 0 ||
            sprite.world_width <= 0.0f || sprite.world_height <= 0.0f) {
            return;
        }
        if (!assets_->is_spawn_id_in_focus_filter(sprite.spawn_id)) {
            return;
        }

        const float world_x = sprite.world_pos.x;
        const float world_y = sprite.world_pos.y;
        const float world_z = static_cast<float>(sprite.world_z);
        const float half_width = sprite.world_width * 0.5f;
        const float height = sprite.world_height;

        SDL_FPoint base_screen{};
        if (!project_world_point(cam, world_x, world_y, world_z, base_screen)) {
            return;
        }

        const float adjusted_y = base_screen.y + boundary_vertical_offset;
        if (base_screen.x + half_width < -boundary_cull_margin ||
            base_screen.x - half_width > static_cast<float>(screen_width_) + boundary_cull_margin ||
            adjusted_y < -boundary_cull_margin ||
            adjusted_y - height > static_cast<float>(screen_height_) + boundary_cull_margin) {
            return;
        }

        if (boundary_min_visible_px > 0.0f) {
            const float largest_dim = std::max(sprite.world_width, sprite.world_height);
            if (largest_dim < boundary_min_visible_px) {
                return;
            }
        }

        const float padding_x = 0.5f / static_cast<float>(sprite.texture_w);
        const float padding_y = 0.5f / static_cast<float>(sprite.texture_h);
        const float u0 = padding_x;
        const float u1 = 1.0f - padding_x;
        const float v0 = padding_y;
        const float v1 = 1.0f - padding_y;

        SDL_Vertex vertices[4]{};
        vertices[0].position = SDL_FPoint{base_screen.x - half_width, adjusted_y - height};
        vertices[1].position = SDL_FPoint{base_screen.x + half_width, adjusted_y - height};
        vertices[2].position = SDL_FPoint{base_screen.x + half_width, adjusted_y};
        vertices[3].position = SDL_FPoint{base_screen.x - half_width, adjusted_y};
        const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
        vertices[0].color = vertices[1].color = vertices[2].color = vertices[3].color = white;
        vertices[0].tex_coord = SDL_FPoint{u0, v0};
        vertices[1].tex_coord = SDL_FPoint{u1, v0};
        vertices[2].tex_coord = SDL_FPoint{u1, v1};
        vertices[3].tex_coord = SDL_FPoint{u0, v1};

        SDL_SetTextureColorMod(sprite.texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(sprite.texture, 255);
        geometry_batcher_->addQuad(sprite.texture, vertices, kQuadIndices, SDL_BLENDMODE_BLEND, depth_from_anchor);
    };

    auto normalize_depth = [](double depth) -> double {
        if (!std::isfinite(depth)) {
            return std::numeric_limits<double>::lowest();
        }
        return depth;
    };
    auto depth_for_traversal = [&](const Assets::ActiveTraversalEntry& entry) -> double {
        if (!entry.asset) {
            return std::numeric_limits<double>::lowest();
        }
        if (std::isfinite(entry.depth_from_anchor)) {
            return normalize_depth(entry.depth_from_anchor);
        }
        return normalize_depth(render_depth::depth_from_anchor(anchor_depth,
                                                               static_cast<double>(entry.asset->world_z()),
                                                               entry.asset->render_depth_bias()));
    };
    auto depth_for_boundary = [&](const DynamicBoundarySystem::BoundarySprite& sprite) -> double {
        return normalize_depth(render_depth::depth_from_anchor(anchor_depth, static_cast<double>(sprite.world_z)));
    };
    const bool runtime_lighting_enabled = assets_->should_render_runtime_lighting();
    const bool need_rendered_asset_list = runtime_lighting_enabled ||
                                          (debug_auto_paths_ && movement_debug_visible_) ||
                                          anchor_point_debug_enabled_;
    const auto& active_traversal = assets_->active_traversal();
    std::vector<Asset*> rendered_assets_for_debug;
    if (need_rendered_asset_list) {
        rendered_assets_for_debug.reserve(active_traversal.size());
    }

    std::size_t traversal_index = 0;
    std::size_t boundary_index = 0;
    while (traversal_index < active_traversal.size() || boundary_index < boundary_sprites.size()) {
        const double next_asset_depth = (traversal_index < active_traversal.size())
            ? depth_for_traversal(active_traversal[traversal_index])
            : std::numeric_limits<double>::lowest();
        const double next_boundary_depth = (boundary_index < boundary_sprites.size())
            ? depth_for_boundary(boundary_sprites[boundary_index])
            : std::numeric_limits<double>::lowest();

        if (traversal_index < active_traversal.size() && next_asset_depth >= next_boundary_depth) {
            const Assets::ActiveTraversalEntry& traversal_entry = active_traversal[traversal_index++];
            Asset* asset = traversal_entry.asset;
            if (!asset || asset->dead) {
                continue;
            }
            if (!assets_->is_asset_in_focus_filter(asset)) {
                continue;
            }

            if (need_rendered_asset_list) {
                rendered_assets_for_debug.push_back(asset);
            }
            composite_renderer_.update(asset, 0.0f);
            const Asset::PerspectiveSample perspective_sample = asset->runtime_perspective_sample();
            const float perspective_scale = perspective_sample.scale;
            const float base_world_z = static_cast<float>(asset->world_z());
            const RuntimeCameraMetrics& camera_metrics = asset->runtime_camera_metrics;
            const bool has_camera_metrics =
                camera_metrics.valid &&
                camera_metrics.frame_id == assets_->frame_id() &&
                camera_metrics.camera_state_version == cam.camera_state_version();
            const double asset_depth_from_anchor = has_camera_metrics
                ? camera_metrics.world_z_depth_from_anchor
                : render_depth::depth_from_anchor(anchor_depth,
                                                  static_cast<double>(asset->world_z()),
                                                  asset->render_depth_bias());

            for (RenderObject& obj : asset->render_package) {
                if (!obj.texture) {
                    continue;
                }
                const float effective_world_z = base_world_z + obj.world_z_offset;
                WarpedMesh mesh{};
                if (!build_perspective_mesh(obj, cam, perspective_scale, effective_world_z, mesh)) {
                    continue;
                }
                if (!mesh.valid) {
                    continue;
                }
                const double draw_depth =
                    asset_depth_from_anchor - static_cast<double>(obj.world_z_offset);
                geometry_batcher_->addQuad(obj.texture,
                                           mesh.vertices.data(),
                                           mesh.indices.data(),
                                           obj.blend_mode,
                                           draw_depth);
            }
            continue;
        }

        if (boundary_index < boundary_sprites.size()) {
            const auto& sprite = boundary_sprites[boundary_index++];
            queue_boundary_sprite(sprite, next_boundary_depth);
            continue;
        }
    }

    std::vector<LayerEffectProcessor::RuntimeLight> runtime_lights;
    if (runtime_lighting_enabled) {
        gather_runtime_lights(cam, rendered_assets_for_debug, runtime_lights);
    }

    auto layer_safe_blend_mode = [](SDL_BlendMode blend_mode) -> SDL_BlendMode {
        if (blend_mode == SDL_BLENDMODE_MOD || blend_mode == SDL_BLENDMODE_MUL) {
            return SDL_BLENDMODE_BLEND;
        }
        return blend_mode;
    };

    auto process_single_scene_layer = [&]() -> bool {
        if (dof_layer_textures_.size() < 1) {
            dof_layer_textures_.resize(1, nullptr);
        }
        if (dof_dark_mask_textures_.size() < 1) {
            dof_dark_mask_textures_.resize(1, nullptr);
        }
        if (dof_lit_textures_.size() < 1) {
            dof_lit_textures_.resize(1, nullptr);
        }

        if (!dof_layer_textures_[0]) {
            dof_layer_textures_[0] = create_target_texture(screen_width_, screen_height_);
        }
        if (!dof_dark_mask_textures_[0]) {
            dof_dark_mask_textures_[0] = create_target_texture(screen_width_, screen_height_);
        }
        if (!dof_lit_textures_[0]) {
            dof_lit_textures_[0] = create_target_texture(screen_width_, screen_height_);
        }

        if (!dof_layer_textures_[0] || !dof_dark_mask_textures_[0] || !dof_lit_textures_[0]) {
            return false;
        }

        SDL_SetRenderTarget(renderer_, dof_layer_textures_[0]);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        geometry_batcher_->for_each_item_far_to_near([&](const GeometryBatcher::DrawItem& draw) {
            if (!draw.texture) {
                return;
            }
            SDL_SetTextureBlendMode(draw.texture, layer_safe_blend_mode(draw.blend_mode));
            SDL_RenderGeometry(renderer_, draw.texture, draw.vertices, 4, kQuadIndices, 6);
        });

        LayerEffectProcessor::LayerLightingParams lighting_params{};
        lighting_params.enabled = runtime_lighting_enabled;
        lighting_params.ambient_color = SDL_Color{18, 20, 24, 255};

        LayerEffectProcessor::LayerFogParams fog_params{};
        if (kBackgroundLayerFogEnabled) {
            float fog_bottom_y = 0.0f;
            if (project_depth_to_floor_screen_y(cam, anchor_depth + max_cull_depth, screen_height_, fog_bottom_y) &&
                fog_bottom_y > 1.0f) {
                fog_params.enabled = true;
                fog_params.normalized_depth = 1.0f;
                fog_params.bottom_y_px = fog_bottom_y;
                fog_params.tint = SDL_Color{222, 232, 242, 255};
            }
        }

        LayerEffectProcessor::LayerBlurParams blur_params{};
        blur_params.enabled = false;
        blur_params.radius_px = 0.0f;
        blur_params.radial_radius_px = 0.0f;

        LayerEffectProcessor::LayerScratchTextures scratch_textures{};
        scratch_textures.dark_mask_texture = dof_dark_mask_textures_[0];

        LayerEffectProcessor::LayerProcessResult layer_result = layer_effect_processor_.process_layer(
            dof_layer_textures_[0],
            dof_lit_textures_[0],
            -max_cull_depth,
            max_cull_depth,
            lighting_params,
            runtime_lights,
            fog_params,
            blur_params,
            scratch_textures);

        SDL_Texture* final_texture = layer_result.final_texture ? layer_result.final_texture : dof_lit_textures_[0];
        if (!final_texture) {
            return false;
        }

        SDL_SetRenderTarget(renderer_, gameplay_target);
        SDL_RenderTexture(renderer_, final_texture, nullptr, nullptr);
        return true;
    };

    if (depth_of_field_enabled ) {
        const double base_layer_interval = std::max(1.0, static_cast<double>(realism.layer_depth_interval));
        const double depth_curve = std::max(0.0, static_cast<double>(realism.layer_depth_curve));
        constexpr int kMaxDofLayers = 512;
        constexpr int kMaxLayersPerSide = kMaxDofLayers / 2;

        auto build_linear_depth_edges = [&](double max_depth) {
            std::vector<double> edges;
            edges.reserve(static_cast<std::size_t>(kMaxLayersPerSide + 1));
            edges.push_back(0.0);
            while (edges.back() < max_depth &&
                   static_cast<int>(edges.size()) - 1 < kMaxLayersPerSide) {
                const double distance = edges.back();
                const double next_distance = std::min(max_depth, distance + base_layer_interval);
                if (next_distance <= distance) {
                    break;
                }
                edges.push_back(next_distance);
            }
            if (edges.size() == 1) {
                edges.push_back(max_depth);
            } else if (edges.back() < max_depth) {
                edges.back() = max_depth;
            }
            return edges;
        };

        auto build_background_depth_edges = [&](double max_depth) {
            std::vector<double> edges;
            edges.reserve(static_cast<std::size_t>(kMaxLayersPerSide + 1));
            edges.push_back(0.0);
            while (edges.back() < max_depth &&
                   static_cast<int>(edges.size()) - 1 < kMaxLayersPerSide) {
                const double distance = edges.back();
                const double t = std::clamp(distance / std::max(1.0, max_depth), 0.0, 1.0);
                const double growth = 1.0 + depth_curve * t * t;
                const double step = std::max(1.0, base_layer_interval * growth);
                const double next_distance = std::min(max_depth, distance + step);
                if (next_distance <= distance) {
                    break;
                }
                edges.push_back(next_distance);
            }
            if (edges.size() == 1) {
                edges.push_back(max_depth);
            } else if (edges.back() < max_depth) {
                edges.back() = max_depth;
            }
            return edges;
        };

        const std::vector<double> foreground_depth_edges = build_linear_depth_edges(max_cull_depth);
        const std::vector<double> background_depth_edges = build_background_depth_edges(max_cull_depth);
        const int foreground_layer_count = std::max(1, static_cast<int>(foreground_depth_edges.size()) - 1);
        const int background_layer_count = std::max(1, static_cast<int>(background_depth_edges.size()) - 1);
        const int layer_count = foreground_layer_count + background_layer_count;

        auto layer_midpoint_depth = [&](int layer_idx) -> double {
            const int clamped_idx = std::clamp(layer_idx, 0, layer_count - 1);
            if (clamped_idx < foreground_layer_count) {
                const int seg = foreground_layer_count - 1 - clamped_idx;
                const double low_abs = foreground_depth_edges[static_cast<std::size_t>(seg)];
                const double high_abs = foreground_depth_edges[static_cast<std::size_t>(seg + 1)];
                return -0.5 * (low_abs + high_abs);
            }
            const int seg = clamped_idx - foreground_layer_count;
            const double low_abs = background_depth_edges[static_cast<std::size_t>(seg)];
            const double high_abs = background_depth_edges[static_cast<std::size_t>(seg + 1)];
            return 0.5 * (low_abs + high_abs);
        };
        auto layer_contains_focus_depth = [&](int layer_idx, double focus_depth) -> bool {
            const int clamped_idx = std::clamp(layer_idx, 0, layer_count - 1);
            double low = 0.0;
            double high = 0.0;
            if (clamped_idx < foreground_layer_count) {
                const int seg = foreground_layer_count - 1 - clamped_idx;
                low = -foreground_depth_edges[static_cast<std::size_t>(seg + 1)];
                high = -foreground_depth_edges[static_cast<std::size_t>(seg)];
            } else {
                const int seg = clamped_idx - foreground_layer_count;
                low = background_depth_edges[static_cast<std::size_t>(seg)];
                high = background_depth_edges[static_cast<std::size_t>(seg + 1)];
            }
            return focus_depth >= low && focus_depth <= high;
        };
        auto depth_to_layer_index = [&](double depth) -> int {
            if (!std::isfinite(depth)) {
                return -1;
            }
            const double clamped = std::clamp(depth, -max_cull_depth, max_cull_depth);
            const double abs_depth = std::fabs(clamped);
            // Keep exact zero depth on the camera/player side of the split so the
            // first fogged/background layer starts strictly behind the player.
            if (clamped <= 0.0) {
                auto upper = std::upper_bound(foreground_depth_edges.begin(), foreground_depth_edges.end(), abs_depth);
                std::ptrdiff_t seg = std::distance(foreground_depth_edges.begin(), upper) - 1;
                seg = std::clamp<std::ptrdiff_t>(seg, 0, static_cast<std::ptrdiff_t>(foreground_layer_count - 1));
                const int idx = foreground_layer_count - 1 - static_cast<int>(seg);
                return std::clamp(idx, 0, layer_count - 1);
            }
            auto upper = std::upper_bound(background_depth_edges.begin(), background_depth_edges.end(), abs_depth);
            std::ptrdiff_t seg = std::distance(background_depth_edges.begin(), upper) - 1;
            seg = std::clamp<std::ptrdiff_t>(seg, 0, static_cast<std::ptrdiff_t>(background_layer_count - 1));
            const int idx = foreground_layer_count + static_cast<int>(seg);
            return std::clamp(idx, 0, layer_count - 1);
        };

        struct LayerSubmission {
            std::vector<const GeometryBatcher::DrawItem*> draws;
            double submitted_depth_sum = 0.0;
            int submitted_depth_count = 0;
            double depth_min = std::numeric_limits<double>::infinity();
            double depth_max = -std::numeric_limits<double>::infinity();
            float bounds_min_x = std::numeric_limits<float>::infinity();
            float bounds_min_y = std::numeric_limits<float>::infinity();
            float bounds_max_x = -std::numeric_limits<float>::infinity();
            float bounds_max_y = -std::numeric_limits<float>::infinity();
        };
        std::vector<LayerSubmission> layers(static_cast<std::size_t>(layer_count));
        geometry_batcher_->for_each_item_far_to_near([&](const GeometryBatcher::DrawItem& item) {
            const int layer_idx = depth_to_layer_index(item.depth);
            if (layer_idx < 0 || layer_idx >= layer_count) {
                return;
            }
            LayerSubmission& layer = layers[static_cast<std::size_t>(layer_idx)];
            layer.draws.push_back(&item);
            if (std::isfinite(item.depth)) {
                layer.submitted_depth_sum += item.depth;
                ++layer.submitted_depth_count;
                layer.depth_min = std::min(layer.depth_min, item.depth);
                layer.depth_max = std::max(layer.depth_max, item.depth);
            }
            for (const SDL_Vertex& vertex : item.vertices) {
                layer.bounds_min_x = std::min(layer.bounds_min_x, vertex.position.x);
                layer.bounds_min_y = std::min(layer.bounds_min_y, vertex.position.y);
                layer.bounds_max_x = std::max(layer.bounds_max_x, vertex.position.x);
                layer.bounds_max_y = std::max(layer.bounds_max_y, vertex.position.y);
            }
        });

        auto destroy_texture_array = [](std::vector<SDL_Texture*>& textures) {
            for (SDL_Texture* tex : textures) {
                if (tex) {
                    SDL_DestroyTexture(tex);
                }
            }
            textures.clear();
        };
        if (static_cast<int>(dof_layer_textures_.size()) != layer_count ||
            static_cast<int>(dof_dark_mask_textures_.size()) != layer_count ||
            static_cast<int>(dof_lit_textures_.size()) != layer_count ||
            static_cast<int>(dof_blur_textures_.size()) != layer_count) {
            destroy_texture_array(dof_layer_textures_);
            destroy_texture_array(dof_dark_mask_textures_);
            destroy_texture_array(dof_lit_textures_);
            destroy_texture_array(dof_blur_textures_);
            dof_layer_textures_.resize(static_cast<std::size_t>(layer_count), nullptr);
            dof_dark_mask_textures_.resize(static_cast<std::size_t>(layer_count), nullptr);
            dof_lit_textures_.resize(static_cast<std::size_t>(layer_count), nullptr);
            dof_blur_textures_.resize(static_cast<std::size_t>(layer_count), nullptr);
        }
        std::vector<int> non_empty_layers;
        non_empty_layers.reserve(static_cast<std::size_t>(layer_count));
        for (int i = 0; i < layer_count; ++i) {
            if (layers[static_cast<std::size_t>(i)].draws.empty()) {
                if (dof_layer_textures_[i]) {
                    SDL_DestroyTexture(dof_layer_textures_[i]);
                    dof_layer_textures_[i] = nullptr;
                }
                if (dof_blur_textures_[i]) {
                    SDL_DestroyTexture(dof_blur_textures_[i]);
                    dof_blur_textures_[i] = nullptr;
                }
                if (dof_lit_textures_[i]) {
                    SDL_DestroyTexture(dof_lit_textures_[i]);
                    dof_lit_textures_[i] = nullptr;
                }
                if (dof_dark_mask_textures_[i]) {
                    SDL_DestroyTexture(dof_dark_mask_textures_[i]);
                    dof_dark_mask_textures_[i] = nullptr;
                }
                continue;
            }
            non_empty_layers.push_back(i);
            if (!dof_layer_textures_[i]) {
                dof_layer_textures_[i] = create_target_texture(screen_width_, screen_height_);
            }
            if (!dof_lit_textures_[i]) {
                dof_lit_textures_[i] = create_target_texture(screen_width_, screen_height_);
            }
            if (!dof_dark_mask_textures_[i]) {
                dof_dark_mask_textures_[i] = create_target_texture(screen_width_, screen_height_);
            }
        }
        const bool need_blur_scratch = depth_of_field_enabled && !non_empty_layers.empty();
        if (need_blur_scratch && !blur_tex_) {
            blur_tex_ = create_target_texture(screen_width_, screen_height_);
        }
        bool dof_targets_ready = !non_empty_layers.empty();
        for (int i : non_empty_layers) {
            if (!dof_layer_textures_[i]) {
                dof_targets_ready = false;
                break;
            }
            if (!dof_lit_textures_[i]) {
                dof_targets_ready = false;
                break;
            }
            if (!dof_dark_mask_textures_[i]) {
                dof_targets_ready = false;
                break;
            }
        }
        if (need_blur_scratch && !blur_tex_) {
            dof_targets_ready = false;
        }
        if (!dof_targets_ready) {
            if (!non_empty_layers.empty()) {
                vibble::log::warn(std::string{"[SceneRenderer] Layer targets unavailable; falling back to single-layer postprocess. SDL error: "} +
                                  SDL_GetError());
            }
            if (!process_single_scene_layer()) {
                SDL_SetRenderTarget(renderer_, gameplay_target);
                geometry_batcher_->flush();
            }
        }

        if (dof_targets_ready) {
            auto render_layer_base = [&](int layer_index) {
                SDL_Texture* layer_tex = dof_layer_textures_[layer_index];
                if (!layer_tex) {
                    return;
                }
                SDL_SetRenderTarget(renderer_, layer_tex);
                SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
                SDL_RenderClear(renderer_);
                for (const GeometryBatcher::DrawItem* draw : layers[static_cast<std::size_t>(layer_index)].draws) {
                    if (!draw) {
                        continue;
                    }
                    SDL_SetTextureBlendMode(draw->texture, layer_safe_blend_mode(draw->blend_mode));
                    SDL_RenderGeometry(renderer_, draw->texture, draw->vertices, 4, kQuadIndices, 6);
                }
            };
            auto light_affects_layer = [&](const LayerEffectProcessor::RuntimeLight& light, const LayerSubmission& layer) {
                const float light_min_x = light.screen_center.x - light.radius_px;
                const float light_min_y = light.screen_center.y - light.radius_px;
                const float light_max_x = light.screen_center.x + light.radius_px;
                const float light_max_y = light.screen_center.y + light.radius_px;
                const bool overlaps_screen =
                    light_max_x >= layer.bounds_min_x &&
                    light_min_x <= layer.bounds_max_x &&
                    light_max_y >= layer.bounds_min_y &&
                    light_min_y <= layer.bounds_max_y;
                bool overlaps_depth = false;
                if (std::isfinite(layer.depth_min) && std::isfinite(layer.depth_max)) {
                    const double depth_radius = std::max(1.0, static_cast<double>(light.radius_px));
                    const double light_depth_min = static_cast<double>(light.world_z) - depth_radius;
                    const double light_depth_max = static_cast<double>(light.world_z) + depth_radius;
                    overlaps_depth = light_depth_max >= layer.depth_min && light_depth_min <= layer.depth_max;
                }
                return overlaps_screen || overlaps_depth;
            };

            SDL_SetRenderTarget(renderer_, gameplay_target);
            // The camera focus point is the depth anchor in this pipeline, so it is always depth 0.
            const double focus_depth = 0.0;
            const int focus_layer = std::clamp(depth_to_layer_index(focus_depth), 0, layer_count - 1);
            const double f_stop = std::max(0.01, static_cast<double>(realism.aperture_f_stop));
            const double focal_length = std::max(0.01, static_cast<double>(realism.focal_length_mm));
            const double max_blur = std::max(0.0, static_cast<double>(realism.max_blur_px));
            const double radial_lens_factor = LayerEffectProcessor::radial_lens_factor_from_optics(focal_length, f_stop);
            const SDL_Point screen_center_i = cam.get_screen_center();
            const SDL_FPoint optical_center{
                std::clamp(static_cast<float>(screen_center_i.x), 0.0f, static_cast<float>(screen_width_)),
                std::clamp(static_cast<float>(screen_center_i.y), 0.0f, static_cast<float>(screen_height_))
            };
            std::vector<SDL_Texture*> final_layer_textures(static_cast<std::size_t>(layer_count), nullptr);
            LayerEffectProcessor::LayerLightingParams lighting_params{};
            lighting_params.enabled = runtime_lighting_enabled;
            lighting_params.ambient_color = SDL_Color{18, 20, 24, 255};

            for (int i : non_empty_layers) {
                render_layer_base(i);

                const LayerSubmission& layer = layers[static_cast<std::size_t>(i)];
                std::vector<LayerEffectProcessor::RuntimeLight> layer_lights;
                layer_lights.reserve(runtime_lights.size());
                for (const LayerEffectProcessor::RuntimeLight& light : runtime_lights) {
                    if (light_affects_layer(light, layer)) {
                        layer_lights.push_back(light);
                    }
                }

                double representative_depth = layer_midpoint_depth(i);
                if (layer.submitted_depth_count > 0) {
                    representative_depth = layer.submitted_depth_sum / static_cast<double>(layer.submitted_depth_count);
                }
                double blur_radius = 0.0;
                if (depth_of_field_enabled && i != focus_layer && !layer_contains_focus_depth(i, focus_depth)) {
                    const double delta = std::fabs(representative_depth - focus_depth);
                    blur_radius = LayerEffectProcessor::coc_blur_radius_from_depth_delta(delta,
                                                                                          max_cull_depth,
                                                                                          focal_length,
                                                                                          f_stop,
                                                                                          max_blur);
                }

                const double depth_norm = std::clamp(std::fabs(representative_depth) / std::max(1.0, max_cull_depth), 0.0, 1.0);
                float effect_quality = static_cast<float>(std::clamp(
                    1.0 - 0.55 * std::pow(depth_norm, 1.35), 0.45, 1.0));
                if (blur_radius < 1.5 && depth_norm < 0.1) {
                    effect_quality = 1.0f;
                }

                LayerEffectProcessor::LayerFogParams fog_params{};
                if (kBackgroundLayerFogEnabled && i >= foreground_layer_count && background_layer_count > 0) {
                    const int seg = i - foreground_layer_count;
                    if (seg >= 0 && seg < background_layer_count) {
                        const double far_depth = background_depth_edges[static_cast<std::size_t>(seg + 1)];
                        float fog_bottom_y = 0.0f;
                        if (project_depth_to_floor_screen_y(cam, anchor_depth + far_depth, screen_height_, fog_bottom_y) &&
                            fog_bottom_y > 1.0f) {
                            fog_params.enabled = true;
                            fog_params.normalized_depth = static_cast<float>(
                                std::clamp(far_depth / std::max(1.0, max_cull_depth), 0.0, 1.0));
                            fog_params.bottom_y_px = fog_bottom_y;
                            fog_params.tint = SDL_Color{222, 232, 242, 255};
                        }
                    }
                }

                LayerEffectProcessor::LayerBlurParams blur_params{};
                if (blur_radius > 0.35) {
                    if (!dof_blur_textures_[i]) {
                        dof_blur_textures_[i] = create_target_texture(screen_width_, screen_height_);
                    }
                    if (dof_blur_textures_[i] && blur_tex_) {
                        const double radial_radius = std::clamp(blur_radius * radial_lens_factor, 0.0, max_blur * 2.0);
                        blur_params.enabled = true;
                        blur_params.radius_px = static_cast<float>(blur_radius);
                        blur_params.optical_center = optical_center;
                        blur_params.radial_radius_px = static_cast<float>(radial_radius);
                        blur_params.quality_scale = effect_quality;
                    }
                }

                const double depth_min = std::isfinite(layer.depth_min) ? layer.depth_min : representative_depth;
                const double depth_max = std::isfinite(layer.depth_max) ? layer.depth_max : representative_depth;

                LayerEffectProcessor::LayerScratchTextures scratch_textures{};
                scratch_textures.dark_mask_texture = dof_dark_mask_textures_[i];
                scratch_textures.blur_texture = blur_params.enabled ? dof_blur_textures_[i] : nullptr;
                scratch_textures.blur_scratch_texture = blur_params.enabled ? blur_tex_ : nullptr;

                LayerEffectProcessor::LayerProcessResult layer_result = layer_effect_processor_.process_layer(
                    dof_layer_textures_[i],
                    dof_lit_textures_[i],
                    depth_min,
                    depth_max,
                    lighting_params,
                    layer_lights,
                    fog_params,
                    blur_params,
                    scratch_textures);

                final_layer_textures[static_cast<std::size_t>(i)] =
                    layer_result.final_texture ? layer_result.final_texture : dof_lit_textures_[i];
            }

            for (auto it = non_empty_layers.rbegin(); it != non_empty_layers.rend(); ++it) {
                const int i = *it;
                SDL_Texture* composite_texture = final_layer_textures[static_cast<std::size_t>(i)];
                if (composite_texture) {
                    SDL_RenderTexture(renderer_, composite_texture, nullptr, nullptr);
                }
            }
        }
    } else {
        if (!process_single_scene_layer()) {
            SDL_SetRenderTarget(renderer_, gameplay_target);
            geometry_batcher_->flush();
        }
    }

    if (gameplay_target) {
        SDL_SetRenderTarget(renderer_, nullptr);
        SDL_SetRenderViewport(renderer_, nullptr);
        SDL_SetRenderClipRect(renderer_, nullptr);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, map_clear_color_.r, map_clear_color_.g, map_clear_color_.b, map_clear_color_.a);
        SDL_RenderClear(renderer_);

        SDL_SetTextureBlendMode(gameplay_target, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(gameplay_target, 255);
        SDL_RenderTexture(renderer_, gameplay_target, nullptr, nullptr);

        if (motion_blur_targets_ready) {
            const int blur_layers = std::min(motion_blur_valid_history_frames_, motion_blur_history_capacity_);
            auto advance_index = [&](int idx) {
                idx += 1;
                if (idx >= kMotionBlurHistoryFrameCount) {
                    idx -= kMotionBlurHistoryFrameCount;
                }
                return idx;
            };
            auto retreat_index = [&](int idx) {
                idx -= 1;
                if (idx < 0) {
                    idx += kMotionBlurHistoryFrameCount;
                }
                return idx;
            };
            auto find_previous_valid_slot = [&](int start_idx) -> int {
                int candidate = start_idx;
                for (int attempt = 0; attempt < kMotionBlurHistoryFrameCount; ++attempt) {
                    if (motion_blur_history_textures_[candidate]) {
                        return candidate;
                    }
                    candidate = retreat_index(candidate);
                }
                return -1;
            };
            auto find_next_valid_slot = [&](int start_idx) -> int {
                int candidate = start_idx;
                for (int attempt = 0; attempt < kMotionBlurHistoryFrameCount; ++attempt) {
                    if (motion_blur_history_textures_[candidate]) {
                        return candidate;
                    }
                    candidate = advance_index(candidate);
                }
                return -1;
            };

            std::array<SDL_Texture*, kMotionBlurHistoryFrameCount> history_layers{};
            int history_layer_count = 0;

            int history_index = motion_blur_history_write_index_;
            for (int age_from_newest = 0; age_from_newest < blur_layers; ++age_from_newest) {
                history_index = retreat_index(history_index);
                history_index = find_previous_valid_slot(history_index);
                if (history_index < 0) {
                    break;
                }

                SDL_Texture* history_texture = motion_blur_history_textures_[history_index];
                if (!history_texture) {
                    continue;
                }
                history_layers[static_cast<std::size_t>(history_layer_count++)] = history_texture;
            }

            for (int age_from_newest = history_layer_count - 1; age_from_newest >= 0; --age_from_newest) {
                SDL_Texture* history_texture = history_layers[static_cast<std::size_t>(age_from_newest)];
                const std::size_t alpha_index =
                    static_cast<std::size_t>(std::min(age_from_newest, kMotionBlurHistoryFrameCount - 1));

                SDL_SetTextureBlendMode(history_texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureAlphaMod(history_texture, kMotionBlurHistoryAlpha[alpha_index]);
                SDL_RenderTexture(renderer_, history_texture, nullptr, nullptr);
                SDL_SetTextureAlphaMod(history_texture, 255);
            }

            int write_slot = find_next_valid_slot(motion_blur_history_write_index_);
            if (write_slot >= 0) {
                SDL_Texture* write_target = motion_blur_history_textures_[write_slot];
                if (write_target) {
                    SDL_SetRenderTarget(renderer_, write_target);
                    SDL_SetRenderViewport(renderer_, nullptr);
                    SDL_SetRenderClipRect(renderer_, nullptr);
                    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
                    SDL_RenderClear(renderer_);
                    SDL_SetTextureBlendMode(gameplay_target, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureAlphaMod(gameplay_target, 255);
                    SDL_RenderTexture(renderer_, gameplay_target, nullptr, nullptr);
                    SDL_SetRenderTarget(renderer_, nullptr);

                    motion_blur_history_write_index_ = advance_index(write_slot);
                    motion_blur_valid_history_frames_ =
                        std::min(motion_blur_valid_history_frames_ + 1, motion_blur_history_capacity_);
                }
            }
        }
    } else {
        SDL_SetRenderTarget(renderer_, nullptr);
    }

    if (debug_auto_paths_ && movement_debug_visible_) {
        refresh_movement_debug_snapshots(rendered_assets_for_debug);
        render_movement_debug_snapshots(cam, screen_width_, screen_height_, rendered_assets_for_debug);
    }

    if (anchor_point_debug_enabled_) {
        render_anchor_debug_markers(renderer_,
                                    cam,
                                    screen_width_,
                                    screen_height_,
                                    rendered_assets_for_debug,
                                    assets_ ? assets_->is_dev_mode() : false);
    }

    // Dev grid overlay is projected against the final scene output.
    if (assets_->dev_grid_overlay_callback_) {
        assets_->dev_grid_overlay_callback_();
    }
}

void SceneRenderer::render_sky_layer(const WarpedScreenGrid& cam) {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    const auto floor_params = cam.compute_floor_depth_params();
    double horizon_y = std::numeric_limits<double>::quiet_NaN();
    if (floor_params.enabled && std::isfinite(floor_params.horizon_screen_y)) {
        // Match the same clamped horizon used for floor warping.
        horizon_y = floor_params.horizon_screen_y;
    } else {
        constexpr double kHalfFovY = 3.14159265358979323846 / 4.0;
        const double pitch_rad = cam.current_pitch_radians();
        const double tan_pitch = std::tan(pitch_rad);
        const double tan_half_fov_y = std::tan(kHalfFovY);
        const double horizon_ndc = (std::isfinite(tan_pitch) && std::isfinite(tan_half_fov_y) && tan_half_fov_y != 0.0)
            ? (tan_pitch / tan_half_fov_y)
            : 0.0;
        horizon_y = static_cast<double>(screen_height_) * 0.5 - horizon_ndc * static_cast<double>(screen_height_) * 0.5;
    }

    if (!std::isfinite(horizon_y)) {
        return;
    }

    const float sky_visible_height =
        std::clamp(static_cast<float>(horizon_y), 0.0f, static_cast<float>(screen_height_));
    if (sky_visible_height <= 0.0f) {
        return;
    }

    if (!ensure_sky_texture() || !sky_texture_) {
        return;
    }

    const float tex_w = static_cast<float>(sky_texture_width_);
    const float tex_h = static_cast<float>(sky_texture_height_);
    if (tex_w <= 0.0f || tex_h <= 0.0f) {
        return;
    }

    const float scale_x = static_cast<float>(screen_width_) / tex_w;
    const float scale_y = sky_visible_height / tex_h;
    const float scale   = std::max(scale_x, scale_y);
    const float target_w = tex_w * scale;
    const float target_h = tex_h * scale;
    if (!std::isfinite(target_h) || target_h <= 0.0f || !std::isfinite(scale)) {
        return;
    }

    SDL_FRect dst{
        (static_cast<float>(screen_width_) - target_w) * 0.5f,
        static_cast<float>(horizon_y) - target_h, target_w, target_h };

    SDL_SetTextureColorMod(sky_texture_, 255, 255, 255);
    SDL_SetTextureAlphaMod(sky_texture_, 255);
    sdl_render::Texture(renderer_, sky_texture_, nullptr, &dst);
}
