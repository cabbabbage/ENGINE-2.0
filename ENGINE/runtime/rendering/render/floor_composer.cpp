#include "rendering/render/floor_composer.hpp"

#include "core/AssetsManager.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/world/tiling/grid_tile.hpp"
#include "gameplay/world/world_grid.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/warped_screen_grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace {

constexpr float kQuadEpsilon = 1.0e-5f;

void destroy_texture(SDL_Texture*& texture) {
    render_diagnostics::destroy_texture(texture);
}

bool project_floor_point_to_screen(const WarpedScreenGrid& cam,
                                   float world_x,
                                   float world_z,
                                   SDL_FPoint& out_screen) {
    SDL_FPoint linear_screen{};
    if (!cam.project_world_point(SDL_FPoint{world_x, 0.0f}, world_z, linear_screen) ||
        !std::isfinite(linear_screen.x) ||
        !std::isfinite(linear_screen.y)) {
        return false;
    }

    linear_screen.y = cam.warp_floor_screen_y(0.0f, linear_screen.y);
    if (!std::isfinite(linear_screen.y)) {
        return false;
    }

    out_screen = linear_screen;
    return true;
}

bool project_floor_grid_point_to_screen(const WarpedScreenGrid& cam,
                                        SDL_Point world_pos,
                                        SDL_FPoint& out_screen) {
    if (!project_floor_point_to_screen(cam,
                                       static_cast<float>(world_pos.x),
                                       static_cast<float>(world_pos.y),
                                       out_screen)) {
        return false;
    }

    return true;
}

float cross(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool on_segment(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& p) {
    return p.x >= std::min(a.x, b.x) - kQuadEpsilon &&
           p.x <= std::max(a.x, b.x) + kQuadEpsilon &&
           p.y >= std::min(a.y, b.y) - kQuadEpsilon &&
           p.y <= std::max(a.y, b.y) + kQuadEpsilon;
}

bool segments_intersect(const SDL_FPoint& a,
                        const SDL_FPoint& b,
                        const SDL_FPoint& c,
                        const SDL_FPoint& d) {
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

    return (d1_zero && on_segment(a, b, c)) ||
           (d2_zero && on_segment(a, b, d)) ||
           (d3_zero && on_segment(c, d, a)) ||
           (d4_zero && on_segment(c, d, b));
}

bool is_convex_quad(const std::array<SDL_FPoint, 4>& points) {
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float area = cross(points[i], points[(i + 1) % 4], points[(i + 2) % 4]);
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

bool enforce_trapezoid(std::array<SDL_FPoint, 4>& points) {
    const bool intersects = segments_intersect(points[0], points[1], points[2], points[3]) ||
                            segments_intersect(points[1], points[2], points[3], points[0]);
    if (!intersects && is_convex_quad(points)) {
        return false;
    }

    std::array<int, 4> idx{0, 1, 2, 3};
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (points[a].y != points[b].y) {
            return points[a].y < points[b].y;
        }
        return points[a].x < points[b].x;
    });

    int tl = idx[0];
    int tr = idx[1];
    int bl = idx[2];
    int br = idx[3];
    if (points[tl].x > points[tr].x) std::swap(tl, tr);
    if (points[bl].x > points[br].x) std::swap(bl, br);
    points = {points[tl], points[tr], points[br], points[bl]};
    return true;
}

struct CachedGroundTileProjection {
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Rect world_rect{0, 0, 0, 0};
    std::uint64_t camera_state_version = 0;
    std::uint64_t last_touched_frame = 0;
    std::array<SDL_Vertex, 4> vertices{};
    bool valid = false;
};

struct GroundTileCacheKey {
    int chunk_i = 0;
    int chunk_j = 0;
    std::uint64_t tile_texture_revision = 0;
    std::uint32_t tile_index = 0;

    bool operator==(const GroundTileCacheKey& other) const {
        return chunk_i == other.chunk_i &&
               chunk_j == other.chunk_j &&
               tile_texture_revision == other.tile_texture_revision &&
               tile_index == other.tile_index;
    }
};

struct GroundTileCacheKeyHash {
    std::size_t operator()(const GroundTileCacheKey& key) const {
        const std::uint64_t h0 = static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.chunk_i));
        const std::uint64_t h1 = static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.chunk_j));
        const std::uint64_t h2 = key.tile_texture_revision;
        const std::uint64_t h3 = key.tile_index;
        std::size_t seed = static_cast<std::size_t>(h0 ^ (h1 << 32));
        seed ^= static_cast<std::size_t>(h2 + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
        seed ^= static_cast<std::size_t>(h3 + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
        return seed;
    }
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

std::unordered_map<GroundTileCacheKey, CachedGroundTileProjection, GroundTileCacheKeyHash>& ground_tile_projection_cache() {
    static std::unordered_map<GroundTileCacheKey, CachedGroundTileProjection, GroundTileCacheKeyHash> cache;
    return cache;
}

void maybe_prune_ground_tile_projection_cache(std::uint64_t current_frame_marker) {
    auto& cache = ground_tile_projection_cache();
    constexpr std::uint64_t kMaxStaleFrames = 120;
    for (auto it = cache.begin(); it != cache.end();) {
        const CachedGroundTileProjection& entry = it->second;
        if (!entry.valid || (current_frame_marker > entry.last_touched_frame &&
                             (current_frame_marker - entry.last_touched_frame) > kMaxStaleFrames)) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
    if (cache.size() > 131072) {
        cache.clear();
    }
}

bool project_room_world_point_to_floor_screen(const WarpedScreenGrid& cam,
                                              const SDL_Point& world_point,
                                              SDL_FPoint& out_screen) {
    return project_floor_point_to_screen(
        cam,
        static_cast<float>(world_point.x),
        static_cast<float>(world_point.y),
        out_screen);
}

void draw_room_and_trail_geometry_overlay(SDL_Renderer* renderer,
                                          const WarpedScreenGrid& cam,
                                          const std::vector<Room*>& rooms) {
    if (!renderer || rooms.empty()) {
        return;
    }

    SDL_BlendMode prev_blend = SDL_BLENDMODE_NONE;
    Uint8 prev_r = 0, prev_g = 0, prev_b = 0, prev_a = 0;
    SDL_GetRenderDrawBlendMode(renderer, &prev_blend);
    SDL_GetRenderDrawColor(renderer, &prev_r, &prev_g, &prev_b, &prev_a);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const Room* room : rooms) {
        if (!room || !room->room_area) {
            continue;
        }
        const std::vector<SDL_Point>& points = room->room_area->get_points();
        if (points.size() < 2) {
            continue;
        }

        const SDL_Color base = room->display_color();
        SDL_SetRenderDrawColor(renderer, base.r, base.g, base.b, 224);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const SDL_Point& a = points[i];
            const SDL_Point& b = points[(i + 1) % points.size()];
            SDL_FPoint sa{};
            SDL_FPoint sb{};
            if (!project_room_world_point_to_floor_screen(cam, a, sa) ||
                !project_room_world_point_to_floor_screen(cam, b, sb)) {
                continue;
            }
            SDL_RenderLine(renderer, sa.x, sa.y, sb.x, sb.y);
        }
    }

    SDL_SetRenderDrawColor(renderer, prev_r, prev_g, prev_b, prev_a);
    SDL_SetRenderDrawBlendMode(renderer, prev_blend);
}

void draw_grid_overlay_points(SDL_Renderer* renderer,
                              const WarpedScreenGrid& cam,
                              int grid_cell_size_px,
                              std::optional<SDL_Point> center_world_override = std::nullopt,
                              std::optional<SDL_FPoint> exact_floor_center = std::nullopt) {
    if (!renderer || grid_cell_size_px <= 0) {
        return;
    }

    const int cell = std::max(1, grid_cell_size_px);
    (void)exact_floor_center;
    auto snap_axis = [cell](float value) -> int {
        const long double ratio = static_cast<long double>(value) / static_cast<long double>(cell);
        const long long snapped = static_cast<long long>(std::llround(ratio)) * static_cast<long long>(cell);
        return static_cast<int>(std::clamp<long long>(snapped, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    };
    SDL_Point center_world{};
    if (center_world_override.has_value()) {
        center_world = *center_world_override;
    } else {
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        const SDL_FPoint mouse_world = cam.screen_to_map(SDL_Point{
            static_cast<int>(std::lround(mouse_x)),
            static_cast<int>(std::lround(mouse_y))});
        if (!std::isfinite(mouse_world.x) || !std::isfinite(mouse_world.y)) {
            return;
        }
        center_world = SDL_Point{snap_axis(mouse_world.x), snap_axis(mouse_world.y)};
    }

    constexpr int kGridPointSizePx = 2;
    constexpr int kGridPointHalf = kGridPointSizePx / 2;
    constexpr int kHighlightPointSizePx = 4;
    constexpr int kHighlightPointHalf = kHighlightPointSizePx / 2;

    SDL_BlendMode prev_blend = SDL_BLENDMODE_NONE;
    Uint8 prev_r = 0, prev_g = 0, prev_b = 0, prev_a = 0;
    SDL_GetRenderDrawBlendMode(renderer, &prev_blend);
    SDL_GetRenderDrawColor(renderer, &prev_r, &prev_g, &prev_b, &prev_a);

    auto [view_min_x, view_min_z, view_max_x, view_max_z] = cam.get_current_view().get_bounds();
    const float view_world_w = std::max(1.0f, static_cast<float>(std::abs(view_max_x - view_min_x)));
    const float view_world_h = std::max(1.0f, static_cast<float>(std::abs(view_max_z - view_min_z)));
    const float base_view_span = std::min(view_world_w, view_world_h);
    // Keep a stable on-screen footprint by basing radius on current visible world span.
    // Requested tuning: render this screen-relative radius at half of its previous size.
    const float radius_world = std::max(static_cast<float>(cell * 2), base_view_span * 0.055f);
    const int radius_cells = std::max(4, static_cast<int>(std::ceil(radius_world / static_cast<float>(cell))));
    const float radius_sq = radius_world * radius_world;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int gy = -radius_cells; gy <= radius_cells; ++gy) {
        for (int gx = -radius_cells; gx <= radius_cells; ++gx) {
            const float dx = static_cast<float>(gx * cell);
            const float dy = static_cast<float>(gy * cell);
            const float dist_sq = dx * dx + dy * dy;
            if (dist_sq > radius_sq) {
                continue;
            }

            const SDL_Point world_point{center_world.x + gx * cell, center_world.y + gy * cell};
            SDL_FPoint screen_point{};
            if (!project_room_world_point_to_floor_screen(cam, world_point, screen_point)) {
                continue;
            }

            const float dist = std::sqrt(std::max(0.0f, dist_sq));
            const float edge_t = std::clamp(dist / radius_world, 0.0f, 1.0f);
            // Fade all the way to transparent at the edge (avoid residual dark/black ring).
            const Uint8 alpha = static_cast<Uint8>(std::lround((1.0f - edge_t) * 180.0f));
            const bool is_cursor_intersection = (gx == 0 && gy == 0);
            if (is_cursor_intersection) {
                SDL_SetRenderDrawColor(renderer, 255, 160, 32, 240);
            } else {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, alpha);
            }

            const int px = static_cast<int>(std::lround(screen_point.x));
            const int py = static_cast<int>(std::lround(screen_point.y));
            const SDL_FRect point_rect{
                static_cast<float>(px - (is_cursor_intersection ? kHighlightPointHalf : kGridPointHalf)),
                static_cast<float>(py - (is_cursor_intersection ? kHighlightPointHalf : kGridPointHalf)),
                static_cast<float>(is_cursor_intersection ? kHighlightPointSizePx : kGridPointSizePx),
                static_cast<float>(is_cursor_intersection ? kHighlightPointSizePx : kGridPointSizePx)};
            SDL_RenderFillRect(renderer, &point_rect);
        }
    }

    SDL_SetRenderDrawColor(renderer, prev_r, prev_g, prev_b, prev_a);
    SDL_SetRenderDrawBlendMode(renderer, prev_blend);
}

} // namespace

GridTileRenderer::GridTileRenderer(Assets* assets)
    : assets_(assets) {}

void GridTileRenderer::invalidate_texture_cache() {
    texture_size_cache_.clear();
    ground_tile_projection_cache().clear();
}

bool GridTileRenderer::fetch_texture_size(SDL_Texture* texture, SDL_FPoint& out_size) {
    if (!texture) {
        return false;
    }

    const auto it = texture_size_cache_.find(texture);
    if (it != texture_size_cache_.end()) {
        out_size = it->second;
        return true;
    }

    float w = 0.0f;
    float h = 0.0f;
    if (!SDL_GetTextureSize(texture, &w, &h) || w <= 0.0f || h <= 0.0f) {
        return false;
    }

    out_size = SDL_FPoint{
        static_cast<float>(std::lround(w)),
        static_cast<float>(std::lround(h))
    };
    texture_size_cache_[texture] = out_size;
    return true;
}

void GridTileRenderer::render(SDL_Renderer* renderer) {
    if (!renderer || !assets_) {
        return;
    }

    render(renderer, assets_->getView(), assets_->world_grid(), nullptr);
}

void GridTileRenderer::render(SDL_Renderer* renderer,
                              const WarpedScreenGrid& cam,
                              const world::WorldGrid& grid) {
    render(renderer, cam, grid, nullptr);
}

void GridTileRenderer::render(SDL_Renderer* renderer,
                              const WarpedScreenGrid& cam,
                              const world::WorldGrid& grid,
                              GeometryBatcher* batcher) {
    if (!renderer) {
        return;
    }

    const auto& chunks = grid.active_chunks();
    if (chunks.empty()) {
        return;
    }

    static std::uint64_t frame_marker = 0;
    ++frame_marker;

    maybe_prune_ground_tile_projection_cache(frame_marker);
    auto& cache = ground_tile_projection_cache();
    const std::uint64_t camera_version = cam.camera_state_version();
    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    static constexpr int indices[6] = {0, 1, 2, 0, 2, 3};

    for (const world::Chunk* chunk : chunks) {
        if (!chunk) {
            continue;
        }

        const std::uint64_t tile_revision = chunk->tile_texture_revision;
        std::uint32_t tile_index = 0;
        for (const auto& tile : chunk->tiles) {
            if (!tile.texture || tile.world_rect.w <= 0 || tile.world_rect.h <= 0) {
                ++tile_index;
                continue;
            }

            const GroundTileCacheKey tile_key{
                chunk->i,
                chunk->j,
                tile_revision,
                tile_index
            };
            auto it = cache.find(tile_key);
            const bool reuse =
                it != cache.end() &&
                cached_ground_tile_projection_matches(it->second,
                                                      renderer,
                                                      tile.texture,
                                                      tile.world_rect,
                                                      camera_version);

            SDL_Vertex verts[4]{};
            if (reuse) {
                verts[0] = it->second.vertices[0];
                verts[1] = it->second.vertices[1];
                verts[2] = it->second.vertices[2];
                verts[3] = it->second.vertices[3];
                it->second.last_touched_frame = frame_marker;
            } else {
                SDL_Point tl{tile.world_rect.x, tile.world_rect.y};
                SDL_Point tr{tile.world_rect.x + tile.world_rect.w, tile.world_rect.y};
                SDL_Point br{tile.world_rect.x + tile.world_rect.w, tile.world_rect.y + tile.world_rect.h};
                SDL_Point bl{tile.world_rect.x, tile.world_rect.y + tile.world_rect.h};

                SDL_FPoint stl{}, str{}, sbr{}, sbl{};
                if (!project_floor_grid_point_to_screen(cam, tl, stl) ||
                    !project_floor_grid_point_to_screen(cam, tr, str) ||
                    !project_floor_grid_point_to_screen(cam, br, sbr) ||
                    !project_floor_grid_point_to_screen(cam, bl, sbl)) {
                    continue;
                }

                std::array<SDL_FPoint, 4> points{stl, str, sbr, sbl};
                enforce_trapezoid(points);

                SDL_FPoint tex_size{};
                if (!fetch_texture_size(tile.texture, tex_size)) {
                    continue;
                }
                const float inv_w = (tex_size.x > 0.0f) ? (1.0f / tex_size.x) : 0.0f;
                const float inv_h = (tex_size.y > 0.0f) ? (1.0f / tex_size.y) : 0.0f;
                const float pad_x = std::clamp(0.5f * inv_w, 0.0f, 0.49f);
                const float pad_y = std::clamp(0.5f * inv_h, 0.0f, 0.49f);

                verts[0].position = points[0];
                verts[1].position = points[1];
                verts[2].position = points[2];
                verts[3].position = points[3];
                for (auto& v : verts) {
                    v.color = white;
                }
                verts[0].tex_coord = SDL_FPoint{pad_x, pad_y};
                verts[1].tex_coord = SDL_FPoint{1.0f - pad_x, pad_y};
                verts[2].tex_coord = SDL_FPoint{1.0f - pad_x, 1.0f - pad_y};
                verts[3].tex_coord = SDL_FPoint{pad_x, 1.0f - pad_y};

                auto& entry = cache[tile_key];
                entry.renderer = renderer;
                entry.texture = tile.texture;
                entry.world_rect = tile.world_rect;
                entry.camera_state_version = camera_version;
                entry.last_touched_frame = frame_marker;
                entry.vertices[0] = verts[0];
                entry.vertices[1] = verts[1];
                entry.vertices[2] = verts[2];
                entry.vertices[3] = verts[3];
                entry.valid = true;
            }

            if (batcher) {
                batcher->addQuad(tile.texture, verts, indices, SDL_BLENDMODE_BLEND, 1000000.0);
            } else {
                render_diagnostics::render_geometry(renderer, tile.texture, verts, 4, indices, 6);
            }
            ++tile_index;
        }
    }
}

FloorComposer::FloorComposer(SDL_Renderer* renderer, Assets* assets)
    : renderer_(renderer),
      assets_(assets),
      tile_renderer_(assets),
      gpu_light_field_processor_(renderer) {}

FloorComposer::~FloorComposer() {
    destroy_owned_textures();
}

void FloorComposer::destroy_owned_textures() {
    destroy_texture(floor_base_texture_);
    destroy_texture(floor_light_mask_texture_);
    destroy_texture(floor_light_falloff_texture_);
    destroy_texture(floor_overlay_texture_);
}

void FloorComposer::set_output_dimensions(int screen_width, int screen_height) {
    const int safe_w = std::max(1, screen_width);
    const int safe_h = std::max(1, screen_height);
    if (safe_w == screen_width_ && safe_h == screen_height_) {
        return;
    }

    screen_width_ = safe_w;
    screen_height_ = safe_h;
    destroy_texture(floor_base_texture_);
    destroy_texture(floor_light_mask_texture_);
    destroy_texture(floor_overlay_texture_);
}

bool FloorComposer::ensure_sized_target(SDL_Texture*& texture) {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }

    if (texture) {
        float wf = 0.0f;
        float hf = 0.0f;
        if (SDL_GetTextureSize(texture, &wf, &hf) &&
            static_cast<int>(std::lround(wf)) == screen_width_ &&
            static_cast<int>(std::lround(hf)) == screen_height_) {
            return true;
        }
        destroy_texture(texture);
    }

    texture = render_diagnostics::create_texture(renderer_,
                                                 SDL_PIXELFORMAT_RGBA8888,
                                                 SDL_TEXTUREACCESS_TARGET,
                                                 screen_width_,
                                                 screen_height_);
    if (!texture) {
        return false;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return true;
}

void FloorComposer::clear_target(SDL_Texture* texture) {
    if (!renderer_ || !texture) {
        return;
    }
    if (!render_diagnostics::set_render_target(renderer_, texture)) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
}

float FloorComposer::compute_horizon_screen_y(const WarpedScreenGrid& cam) const {
    const auto floor_params = cam.compute_floor_depth_params();
    if (floor_params.enabled && std::isfinite(floor_params.horizon_screen_y)) {
        return std::clamp(static_cast<float>(floor_params.horizon_screen_y),
                          0.0f,
                          static_cast<float>(screen_height_));
    }

    constexpr double kHalfFovY = 3.14159265358979323846 / 4.0;
    const double pitch_rad = cam.current_pitch_radians();
    const double tan_pitch = std::tan(pitch_rad);
    const double tan_half_fov_y = std::tan(kHalfFovY);
    const double horizon_ndc =
        (std::isfinite(tan_pitch) && std::isfinite(tan_half_fov_y) && tan_half_fov_y != 0.0)
            ? (tan_pitch / tan_half_fov_y)
            : 0.0;
    const double horizon_y =
        static_cast<double>(screen_height_) * 0.5 -
        horizon_ndc * static_cast<double>(screen_height_) * 0.5;
    if (!std::isfinite(horizon_y)) {
        return static_cast<float>(screen_height_);
    }
    return std::clamp(static_cast<float>(horizon_y), 0.0f, static_cast<float>(screen_height_));
}

SDL_Texture* FloorComposer::ensure_floor_light_falloff_texture() {
    if (!renderer_) {
        return nullptr;
    }
    if (floor_light_falloff_texture_) {
        return floor_light_falloff_texture_;
    }

    constexpr int kTextureSize = 192;
    SDL_Texture* texture = render_diagnostics::create_texture(renderer_,
                                                              SDL_PIXELFORMAT_RGBA8888,
                                                              SDL_TEXTUREACCESS_STREAMING,
                                                              kTextureSize,
                                                              kTextureSize);
    if (!texture) {
        return nullptr;
    }

    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(texture, nullptr, &pixels, &pitch) || !pixels || pitch <= 0) {
        render_diagnostics::destroy_texture(texture);
        return nullptr;
    }

    auto* base = static_cast<std::uint8_t*>(pixels);
    const SDL_PixelFormatDetails* pixel_format = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA8888);
    if (!pixel_format) {
        SDL_UnlockTexture(texture);
        render_diagnostics::destroy_texture(texture);
        return nullptr;
    }

    const float center = (static_cast<float>(kTextureSize) - 1.0f) * 0.5f;
    const float max_radius = std::max(1.0f, center);
    for (int y = 0; y < kTextureSize; ++y) {
        auto* row = reinterpret_cast<Uint32*>(base + (pitch * y));
        for (int x = 0; x < kTextureSize; ++x) {
            const float dx = (static_cast<float>(x) - center) / max_radius;
            const float dy = (static_cast<float>(y) - center) / max_radius;
            const float distance = std::sqrt((dx * dx) + (dy * dy));
            const float radial = std::clamp(1.0f - distance, 0.0f, 1.0f);
            const float smoothed = radial * radial * (3.0f - (2.0f * radial));
            const float curved = std::pow(smoothed, 1.6f);
            const Uint8 value = static_cast<Uint8>(std::clamp(
                static_cast<int>(std::lround(curved * 255.0f)),
                0,
                255));
            row[x] = SDL_MapRGBA(pixel_format, nullptr, value, value, value, value);
        }
    }

    SDL_UnlockTexture(texture);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    floor_light_falloff_texture_ = texture;
    return floor_light_falloff_texture_;
}

SDL_Texture* FloorComposer::compose_gpu(const WarpedScreenGrid& cam,
                                        const world::WorldGrid& grid,
                                        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
                                        bool runtime_lighting_enabled,
                                        double max_cull_depth,
                                        SDL_Color clear_color,
                                        bool render_floor_tiles) {
    has_floor_dark_mask_ = false;
    if (!renderer_ ||
        !ensure_sized_target(floor_base_texture_) ||
        !ensure_sized_target(floor_light_mask_texture_) ||
        !ensure_sized_target(floor_overlay_texture_)) {
        return nullptr;
    }

    const float horizon_y = compute_horizon_screen_y(cam);
    const int floor_top = std::clamp(static_cast<int>(std::floor(horizon_y)), 0, screen_height_);
    const int floor_height = std::max(0, screen_height_ - floor_top);
    SDL_Rect floor_clip{0, floor_top, screen_width_, floor_height};

    clear_target(floor_base_texture_);
    if (!render_diagnostics::set_render_target(renderer_, floor_base_texture_)) {
        return nullptr;
    }
    if (floor_height > 0) {
        SDL_SetRenderClipRect(renderer_, &floor_clip);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, clear_color.r, clear_color.g, clear_color.b, 255);
        const SDL_FRect full_rect{0.0f, 0.0f, static_cast<float>(screen_width_), static_cast<float>(screen_height_)};
        SDL_RenderFillRect(renderer_, &full_rect);
        if (render_floor_tiles) {
            tile_renderer_.render(renderer_, cam, grid, nullptr);
        }
        SDL_SetRenderClipRect(renderer_, nullptr);
    }

    clear_target(floor_overlay_texture_);
    if (render_diagnostics::set_render_target(renderer_, floor_overlay_texture_)) {
        if (floor_height > 0) {
            SDL_SetRenderClipRect(renderer_, &floor_clip);
            if (assets_ && assets_->is_dev_mode()) {
                draw_room_and_trail_geometry_overlay(renderer_, cam, assets_->rooms());
                if (assets_->dev_grid_overlay_enabled()) {
                    const Assets::DevGridOverlayContext overlay_ctx = assets_->dev_grid_overlay_context();
                    if (overlay_ctx.kind == Assets::DevGridOverlayKind::FloorCenteredOnSelectedPoint &&
                        overlay_ctx.has_selected_point_center) {
                        draw_grid_overlay_points(renderer_,
                                                 cam,
                                                 assets_->dev_grid_overlay_cell_size_px(),
                                                 overlay_ctx.snapped_floor_xz,
                                                 overlay_ctx.exact_floor_xz);
                    } else if (overlay_ctx.kind != Assets::DevGridOverlayKind::XYPlaneAtAssetDepth) {
                        draw_grid_overlay_points(renderer_, cam, assets_->dev_grid_overlay_cell_size_px());
                    }
                }
            }
            SDL_SetRenderClipRect(renderer_, nullptr);
        }
    }

    if (!runtime_lighting_enabled) {
        return floor_base_texture_;
    }

    std::vector<LayerEffectProcessor::GpuRadialLight> gpu_lights;
    gpu_lights.reserve(runtime_lights.size());
    const float floor_light_cull_depth = static_cast<float>(std::max(1.0, max_cull_depth));
    for (const auto& light : runtime_lights) {
        if (!light.has_floor_projection ||
            !std::isfinite(light.floor_world_x) ||
            !std::isfinite(light.floor_world_z) ||
            !std::isfinite(light.world_height) ||
            !std::isfinite(light.floor_screen_center.x) ||
            !std::isfinite(light.floor_screen_center.y)) {
            continue;
        }

        const float abs_depth = std::fabs(light.world_z);
        if (abs_depth > floor_light_cull_depth) {
            continue;
        }

        const render_internal::FloorLightContact contact =
            render_internal::resolve_floor_light_contact(light.floor_world_x,
                                                         light.floor_world_z,
                                                         light.floor_world_x,
                                                         light.floor_world_z,
                                                         light.world_height);
        if (!contact.valid) {
            continue;
        }

        const float radius_world = std::max(1.0f, light.radius_world > 0.0f ? light.radius_world : light.radius_px);
        const float height_weight = render_internal::floor_light_height_weight(contact.world_height, radius_world);
        const float height_spread = render_internal::floor_light_height_spread_scale(contact.world_height, radius_world);

        float radius_x_px = 0.0f;
        float radius_y_px = 0.0f;
        if (!render_internal::sample_floor_light_footprint_axes_px(cam,
                                                                   contact,
                                                                   light.floor_screen_center,
                                                                   radius_world,
                                                                   height_spread,
                                                                   radius_x_px,
                                                                   radius_y_px)) {
            continue;
        }

        const float depth_weight = render_internal::floor_light_depth_weight(abs_depth, floor_light_cull_depth);
        const float opacity = std::clamp(light.opacity > 0.0f ? light.opacity : 1.0f, 0.0f, 1.0f);
        const float effective_intensity = light.intensity * depth_weight * height_weight;
        if (!std::isfinite(effective_intensity) || effective_intensity < 0.02f) {
            continue;
        }

        LayerEffectProcessor::GpuRadialLight draw{};
        draw.center = light.floor_screen_center;
        draw.color = light.color;
        draw.radius_x_px = std::max(1.0f, radius_x_px);
        draw.radius_y_px = std::max(1.0f, radius_y_px);
        draw.intensity = effective_intensity;
        draw.opacity = opacity;
        draw.falloff = std::max(0.05f, light.falloff);
        gpu_lights.push_back(draw);
    }

    if (!gpu_light_field_processor_.render_gpu_light_field(floor_light_mask_texture_,
                                                           SDL_Color{36, 38, 42, 255},
                                                           gpu_lights,
                                                           SDL_BLENDMODE_ADD)) {
        return floor_base_texture_;
    }

    if (floor_top > 0) {
        if (!render_diagnostics::set_render_target(renderer_, floor_light_mask_texture_)) {
            return floor_base_texture_;
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        const SDL_FRect top_rect{
            0.0f,
            0.0f,
            static_cast<float>(screen_width_),
            static_cast<float>(floor_top)};
        SDL_RenderFillRect(renderer_, &top_rect);
    }

    has_floor_dark_mask_ = true;
    return floor_base_texture_;
}

SDL_Texture* FloorComposer::compose(const WarpedScreenGrid& cam,
                                    const world::WorldGrid& grid,
                                    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
                                    bool runtime_lighting_enabled,
                                    double max_cull_depth,
                                    SDL_Color clear_color,
                                    bool render_floor_tiles) {
    has_floor_dark_mask_ = false;
    if (!renderer_ ||
        !ensure_sized_target(floor_base_texture_) ||
        !ensure_sized_target(floor_light_mask_texture_) ||
        !ensure_sized_target(floor_overlay_texture_)) {
        return nullptr;
    }

    const float horizon_y = compute_horizon_screen_y(cam);
    const int floor_top = std::clamp(static_cast<int>(std::floor(horizon_y)), 0, screen_height_);
    const int floor_height = std::max(0, screen_height_ - floor_top);
    SDL_Rect floor_clip{0, floor_top, screen_width_, floor_height};

    clear_target(floor_base_texture_);
    if (!render_diagnostics::set_render_target(renderer_, floor_base_texture_)) {
        return nullptr;
    }
    if (floor_height > 0) {
        SDL_SetRenderClipRect(renderer_, &floor_clip);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, clear_color.r, clear_color.g, clear_color.b, 255);
        const SDL_FRect full_rect{0.0f, 0.0f, static_cast<float>(screen_width_), static_cast<float>(screen_height_)};
        SDL_RenderFillRect(renderer_, &full_rect);
        if (render_floor_tiles) {
            tile_renderer_.render(renderer_, cam, grid, nullptr);
        }
        SDL_SetRenderClipRect(renderer_, nullptr);
    }

    clear_target(floor_overlay_texture_);
    if (render_diagnostics::set_render_target(renderer_, floor_overlay_texture_)) {
        if (floor_height > 0) {
            SDL_SetRenderClipRect(renderer_, &floor_clip);
            if (assets_ && assets_->is_dev_mode()) {
                draw_room_and_trail_geometry_overlay(renderer_, cam, assets_->rooms());
                if (assets_->dev_grid_overlay_enabled()) {
                    const Assets::DevGridOverlayContext overlay_ctx = assets_->dev_grid_overlay_context();
                    if (overlay_ctx.kind == Assets::DevGridOverlayKind::FloorCenteredOnSelectedPoint &&
                        overlay_ctx.has_selected_point_center) {
                        draw_grid_overlay_points(renderer_,
                                                 cam,
                                                 assets_->dev_grid_overlay_cell_size_px(),
                                                 overlay_ctx.snapped_floor_xz,
                                                 overlay_ctx.exact_floor_xz);
                    } else if (overlay_ctx.kind != Assets::DevGridOverlayKind::XYPlaneAtAssetDepth) {
                        draw_grid_overlay_points(renderer_, cam, assets_->dev_grid_overlay_cell_size_px());
                    }
                }
            }
            SDL_SetRenderClipRect(renderer_, nullptr);
        }
    }

    if (!runtime_lighting_enabled) {
        return floor_base_texture_;
    }

    clear_target(floor_light_mask_texture_);
    if (!render_diagnostics::set_render_target(renderer_, floor_light_mask_texture_)) {
        return floor_base_texture_;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    if (floor_height > 0) {
        SDL_SetRenderClipRect(renderer_, &floor_clip);
        SDL_SetRenderDrawColor(renderer_, 36, 38, 42, 255);
        const SDL_FRect full_rect{0.0f, 0.0f, static_cast<float>(screen_width_), static_cast<float>(screen_height_)};
        SDL_RenderFillRect(renderer_, &full_rect);

        const float floor_light_cull_depth = static_cast<float>(std::max(1.0, max_cull_depth));
        SDL_Texture* falloff_texture = ensure_floor_light_falloff_texture();
        if (falloff_texture) {
            SDL_SetTextureBlendMode(falloff_texture, SDL_BLENDMODE_ADD);
            SDL_Color last_color{0, 0, 0, 0};
            Uint8 last_alpha = 0;

            for (const auto& light : runtime_lights) {
                if (!light.has_floor_projection ||
                    !std::isfinite(light.floor_world_x) ||
                    !std::isfinite(light.floor_world_z) ||
                    !std::isfinite(light.world_height) ||
                    !std::isfinite(light.floor_screen_center.x) ||
                    !std::isfinite(light.floor_screen_center.y)) {
                    continue;
                }

                const float abs_depth = std::fabs(light.world_z);
                if (abs_depth > floor_light_cull_depth) {
                    continue;
                }

                const render_internal::FloorLightContact contact =
                    render_internal::resolve_floor_light_contact(light.floor_world_x,
                                                                 light.floor_world_z,
                                                                 light.floor_world_x,
                                                                 light.floor_world_z,
                                                                 light.world_height);
                if (!contact.valid) {
                    continue;
                }

                const float radius_world = std::max(1.0f, light.radius_world > 0.0f ? light.radius_world : light.radius_px);
                const float height_weight = render_internal::floor_light_height_weight(contact.world_height, radius_world);
                const float height_spread = render_internal::floor_light_height_spread_scale(contact.world_height, radius_world);

                float radius_x_px = 0.0f;
                float radius_y_px = 0.0f;
                if (!render_internal::sample_floor_light_footprint_axes_px(cam,
                                                                           contact,
                                                                           light.floor_screen_center,
                                                                           radius_world,
                                                                           height_spread,
                                                                           radius_x_px,
                                                                           radius_y_px)) {
                    continue;
                }

                const float depth_weight = render_internal::floor_light_depth_weight(abs_depth, floor_light_cull_depth);
                const float effective_intensity = light.intensity * depth_weight * height_weight;
                if (effective_intensity < 0.02f) {
                    continue;
                }

                if (last_color.r != light.color.r || last_color.g != light.color.g || last_color.b != light.color.b) {
                    SDL_SetTextureColorMod(falloff_texture, light.color.r, light.color.g, light.color.b);
                    last_color = SDL_Color{light.color.r, light.color.g, light.color.b, 255};
                }

                const int pass_count = std::clamp(static_cast<int>(std::ceil(effective_intensity)), 1, 4);
                const float per_pass = effective_intensity / static_cast<float>(pass_count);
                const Uint8 alpha = static_cast<Uint8>(std::clamp(
                    static_cast<int>(std::lround(per_pass * 192.0f)),
                    0,
                    255));
                if (alpha == 0) {
                    continue;
                }

                if (last_alpha != alpha) {
                    SDL_SetTextureAlphaMod(falloff_texture, alpha);
                    last_alpha = alpha;
                }

                const SDL_FRect dst_rect{
                    light.floor_screen_center.x - radius_x_px,
                    light.floor_screen_center.y - radius_y_px,
                    radius_x_px * 2.0f,
                    radius_y_px * 2.0f
                };
                for (int pass = 0; pass < pass_count; ++pass) {
                    render_diagnostics::render_texture(renderer_, falloff_texture, nullptr, &dst_rect);
                }
            }

            SDL_SetTextureAlphaMod(falloff_texture, 255);
            SDL_SetTextureColorMod(falloff_texture, 255, 255, 255);
        }

        SDL_SetRenderClipRect(renderer_, nullptr);
    }
    has_floor_dark_mask_ = true;

    return floor_base_texture_;
}
