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
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <sstream>
#include <utility>
#include <vector>

#include <SDL3_image/SDL_image.h>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_library.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/dynamic_fog_system.hpp"
#include "rendering/render/dynamic_boundary_system.hpp"
#include "animation/animation_update.hpp"
#include "gameplay/world/tiling/grid_tile.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "utils/AnchorPointResolver.hpp"
#include "utils/log.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"

namespace {
bool enforce_trapezoid(std::array<SDL_FPoint, 4>& points);
}

// ============================================================================
// GeometryBatcher Implementation
// ============================================================================

GeometryBatcher::GeometryBatcher(SDL_Renderer* renderer)
    : renderer_(renderer) {
    // Pre-allocate for estimated 10,000 quads (40,000 vertices, 60,000 indices)
    vertex_buffer_.reserve(40000);
    index_buffer_.reserve(60000);
    draw_list_.reserve(10000);
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

    draw_list_.push_back(item);
}

void GeometryBatcher::flush() {
    if (draw_list_.empty()) {
        last_flush_cpu_ms_ = 0.0;
        return;
    }

    const auto flush_start = std::chrono::steady_clock::now();

    draw_call_count_ = 0;
    total_vertices_ = 0;

    // Static quad indices pattern (0,1,2, 0,2,3)
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    // Enforce a deterministic painter's order before emitting draw batches.
    // This protects against upstream submit-order jitter (e.g. mixed systems
    // or per-asset multi-quad offsets) and keeps the renderer depth-correct.
    std::stable_sort(draw_list_.begin(), draw_list_.end(), [](const DrawItem& lhs, const DrawItem& rhs) {
        const bool lhs_finite = std::isfinite(lhs.depth);
        const bool rhs_finite = std::isfinite(rhs.depth);
        if (lhs_finite != rhs_finite) {
            return lhs_finite; // finite depths first
        }
        if (!lhs_finite && !rhs_finite) {
            return false; // preserve submit order among invalid depths
        }
        if (lhs.depth == rhs.depth) {
            return false; // preserve submit order for equal depths
        }
        return lhs.depth > rhs.depth; // descending depth
    });

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

    SDL_Texture* current_texture = nullptr;
    SDL_BlendMode current_blend = SDL_BLENDMODE_BLEND;

    double previous_depth = draw_list_.front().depth;
    bool has_previous_depth = false;

    for (const auto& quad : draw_list_) {
#ifndef NDEBUG
        if (has_previous_depth && quad.depth > previous_depth) {
            SDL_assert(!"GeometryBatcher::flush draw_list_ depth is not monotonic (expected descending order)");
        }
#endif
        previous_depth = quad.depth;
        has_previous_depth = true;

        if (!current_texture) {
            current_texture = quad.texture;
            current_blend = quad.blend_mode;
        }

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
    }

    emit_current_batch(current_texture, current_blend);

    const auto flush_end = std::chrono::steady_clock::now();
    last_flush_cpu_ms_ = std::chrono::duration<double, std::milli>(flush_end - flush_start).count();
}

void GeometryBatcher::clear() {
    draw_list_.clear();
    draw_call_count_ = 0;
    total_vertices_ = 0;
    last_flush_cpu_ms_ = 0.0;
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

            SDL_Vertex verts[4]{};
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

            if (batcher) {
                // Ground tiles sit behind world assets; use a large depth so batch order stays monotonic.
                batcher->addQuad(tile.texture, verts, indices, SDL_BLENDMODE_BLEND, 1'000'000.0);
            } else {
                SDL_RenderGeometry(renderer, tile.texture, verts, 4, indices, 6);
            }
        }
    }

    if (batcher) {
        batcher->flush();
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
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
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
                                 int screen_width,
                                 int screen_height,
                                 const std::vector<Asset*>& assets) {
    if (!renderer) {
        return;
    }

    const SDL_Color kFlatColor{255, 32, 32, 220};
    const SDL_Color kFinalColor{48, 128, 255, 255};
    constexpr int kFlatRadiusPx = 5;
    constexpr int kFinalRadiusPx = 3;

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
    const float world_x = static_cast<float>(rect.x);
    const float world_y = static_cast<float>(rect.y);
    const SDL_FPoint current_position{world_x, world_y};

    // Render package dimensions already include the per-grid-point perspective scale;
    // strip it off so the projection step is the sole place that applies perspective.
    const float safe_perspective =
        (std::isfinite(perspective_scale) && perspective_scale > 0.0f)
            ? perspective_scale
            : 1.0f;
    const float current_scale = safe_perspective;
    const std::uint64_t current_camera_version = cam.camera_state_version();
    constexpr float kScaleMatchEpsilon = 1e-4f;
    if (!obj.mesh_dirty &&
        obj.cached_vertices.size() == 4 &&
        obj.cached_indices.size() == 6 &&
        std::fabs(obj.cached_scale - current_scale) < kScaleMatchEpsilon &&
        std::fabs(obj.cached_world_z - base_world_z) < kScaleMatchEpsilon &&
        obj.cached_position.x == current_position.x &&
        obj.cached_position.y == current_position.y &&
        obj.cached_camera_state_version == current_camera_version) {
        mesh.vertices = obj.cached_vertices;
        mesh.indices = obj.cached_indices;
        return true;
    }
    const float world_width = static_cast<float>(rect.w) / safe_perspective;
    const float world_height = static_cast<float>(rect.h) / safe_perspective;
    const float half_width = world_width * 0.5f;
    const float height = world_height;
    const float base_z = base_world_z;
    if (!(std::isfinite(world_x) && std::isfinite(world_y) && std::isfinite(half_width) && std::isfinite(height))) {
        return false;
    }

    int atlas_w = 0;
    int atlas_h = 0;
    float atlas_wf = 0.0f;
    float atlas_hf = 0.0f;
    if (!SDL_GetTextureSize(obj.texture, &atlas_wf, &atlas_hf)) {
        return false;
    }
    atlas_w = static_cast<int>(std::lround(atlas_wf));
    atlas_h = static_cast<int>(std::lround(atlas_hf));
    if (atlas_w <= 0 || atlas_h <= 0) {
        return false;
    }

    int tex_w = obj.texture_w;
    int tex_h = obj.texture_h;
    if (!obj.has_texture_size) {
        tex_w = atlas_w;
        tex_h = atlas_h;
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

    // Anchor the quad to the real-world bottom corners. Build a perfect rectangle whose
    // width is the bottom edge and whose height preserves the source aspect ratio.
    SDL_FPoint screen_bl{};
    SDL_FPoint screen_br{};
    if (!project_world_point(cam, world_x - half_width, world_y, base_z, screen_bl) ||
        !project_world_point(cam, world_x + half_width, world_y, base_z, screen_br)) {
        return false;
    }

    const float bottom_dx = screen_br.x - screen_bl.x;
    const float bottom_dy = screen_br.y - screen_bl.y;
    const float bottom_len = std::hypot(bottom_dx, bottom_dy);
    if (bottom_len < 1e-5f) {
        return false; // degenerate projection
    }

    const float aspect = (tex_w > 0 && tex_h > 0)
        ? static_cast<float>(tex_h) / static_cast<float>(tex_w)
        : (rect.w != 0 ? static_cast<float>(rect.h) / static_cast<float>(rect.w) : 1.0f);
    float screen_height = bottom_len * aspect;
    if (!std::isfinite(screen_height) || screen_height <= 0.0f) {
        screen_height = std::abs(screen_height);
        if (screen_height <= 0.0f) {
            screen_height = world_height; // fallback to unwarped height if projection misbehaves
        }
    }

    // Perpendicular unit vector to the bottom edge
    const float nx = -bottom_dy / bottom_len;
    const float ny =  bottom_dx / bottom_len;

    // Two candidate orientations; pick the one that places the top above the bottom in screen space.
    const SDL_FPoint cand_tl_a{screen_bl.x + nx * screen_height, screen_bl.y + ny * screen_height};
    const SDL_FPoint cand_tr_a{screen_br.x + nx * screen_height, screen_br.y + ny * screen_height};
    const SDL_FPoint cand_tl_b{screen_bl.x - nx * screen_height, screen_bl.y - ny * screen_height};
    const SDL_FPoint cand_tr_b{screen_br.x - nx * screen_height, screen_br.y - ny * screen_height};

    const float avg_bottom_y = 0.5f * (screen_bl.y + screen_br.y);
    const float avg_top_a = 0.5f * (cand_tl_a.y + cand_tr_a.y);
    const float avg_top_b = 0.5f * (cand_tl_b.y + cand_tr_b.y);

    bool a_is_above = avg_top_a < avg_bottom_y;
    bool b_is_above = avg_top_b < avg_bottom_y;

    SDL_FPoint screen_tl{};
    SDL_FPoint screen_tr{};
    if (a_is_above && (!b_is_above || avg_top_a <= avg_top_b)) {
        screen_tl = cand_tl_a;
        screen_tr = cand_tr_a;
    } else if (b_is_above && (!a_is_above || avg_top_b <= avg_top_a)) {
        screen_tl = cand_tl_b;
        screen_tr = cand_tr_b;
    } else {
        // Neither is above; choose the one closer to being above (smaller avg y).
        if (avg_top_a <= avg_top_b) {
            screen_tl = cand_tl_a;
            screen_tr = cand_tr_a;
        } else {
            screen_tl = cand_tl_b;
            screen_tr = cand_tr_b;
        }
    }

    mesh.vertices.clear();
    mesh.indices.clear();
    mesh.vertices.reserve(4);
    mesh.indices.reserve(6);

    const SDL_FColor vertex_color{
        obj.color_mod.r / 255.0f,
        obj.color_mod.g / 255.0f,
        obj.color_mod.b / 255.0f,
        obj.color_mod.a / 255.0f};

    SDL_Vertex vtx_tl{};
    vtx_tl.position = screen_tl;
    vtx_tl.color = vertex_color;
    vtx_tl.tex_coord = SDL_FPoint{u0, v0};

    SDL_Vertex vtx_tr{};
    vtx_tr.position = screen_tr;
    vtx_tr.color = vertex_color;
    vtx_tr.tex_coord = SDL_FPoint{u1, v0};

    SDL_Vertex vtx_br{};
    vtx_br.position = screen_br;
    vtx_br.color = vertex_color;
    vtx_br.tex_coord = SDL_FPoint{u1, v1};

    SDL_Vertex vtx_bl{};
    vtx_bl.position = screen_bl;
    vtx_bl.color = vertex_color;
    vtx_bl.tex_coord = SDL_FPoint{u0, v1};

    mesh.vertices.push_back(vtx_tl);
    mesh.vertices.push_back(vtx_tr);
    mesh.vertices.push_back(vtx_br);
    mesh.vertices.push_back(vtx_bl);

    mesh.indices = {0, 1, 2, 0, 2, 3};
    obj.cached_vertices = mesh.vertices;
    obj.cached_indices = mesh.indices;
    obj.cached_position = current_position;
    obj.cached_world_z = base_z;
    obj.cached_scale = current_scale;
    obj.cached_camera_state_version = current_camera_version;
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
  texture_load_queue_(std::make_unique<texture_loading::TextureLoadQueue>(renderer)),
  sky_texture_path_(std::filesystem::path("resources") / "misc_content" / "sky.png"),
  composite_renderer_(renderer, assets),
  dynamic_fog_system_(std::make_unique<DynamicFogSystem>()),
  dynamic_boundary_system_(std::make_unique<DynamicBoundarySystem>())
{

    map_clear_color_ = SDL_Color{69, 101, 74, 255};

    // Initialize dynamic fog system
    if (dynamic_fog_system_) {
        if (!dynamic_fog_system_->initialize(renderer_)) {
            vibble::log::warn("[SceneRenderer] Failed to initialize dynamic fog system");
        }
    }

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

    if (const char* override_frames = std::getenv("VIBBLE_DEPTHCUE_WARMUP_FRAMES")) {
        const int v = std::atoi(override_frames);
        if (v >= 0 && v <= 120) {
            depthcue_warmup_frames_ = static_cast<std::uint32_t>(v);
        }
    }
    std::cout<<"[SceneRenderer] Init complete. Depth-cue warmup frames: "<<depthcue_warmup_frames_<<std::endl;
}

SceneRenderer::~SceneRenderer() {
    destroy_sky_texture();
    if (scene_composite_tex_) { SDL_DestroyTexture(scene_composite_tex_); scene_composite_tex_ = nullptr; }
    if (postprocess_tex_)     { SDL_DestroyTexture(postprocess_tex_);     postprocess_tex_     = nullptr; }
    if (blur_tex_)            { SDL_DestroyTexture(blur_tex_);            blur_tex_            = nullptr; }
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

texture_loading::TextureLoadQueue* SceneRenderer::get_texture_load_queue() const {
    return texture_load_queue_.get();
}

void SceneRenderer::set_movement_debug_enabled(bool enabled) {
    debug_auto_paths_ = enabled;
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

void SceneRenderer::render() {
    static int s_boundary_render_debug_counter = 0;
    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, map_clear_color_.r, map_clear_color_.g, map_clear_color_.b, map_clear_color_.a);
    SDL_RenderClear(renderer_);
    WarpedScreenGrid& cam = assets_->getView();
    world::WorldGrid& grid = assets_->world_grid();
    render_sky_layer(cam, assets_->depth_effects_enabled());

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
    const float boundary_vertical_offset = DynamicBoundarySystem::vertical_offset();
    const float boundary_cull_margin = 64.0f;
    std::size_t queued_boundary_sprites = 0;
    float min_boundary_width = std::numeric_limits<float>::max();
    float max_boundary_width = 0.0f;
    float min_boundary_height = std::numeric_limits<float>::max();
    float max_boundary_height = 0.0f;

    auto queue_boundary_sprite = [&](const DynamicBoundarySystem::BoundarySprite& sprite, double depth) {
        if (!sprite.texture ||
            sprite.texture_w <= 0 || sprite.texture_h <= 0 ||
            sprite.world_width <= 0.0f || sprite.world_height <= 0.0f) {
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
        geometry_batcher_->addQuad(sprite.texture, vertices, kQuadIndices, SDL_BLENDMODE_BLEND, depth);
        ++queued_boundary_sprites;
        min_boundary_width = std::min(min_boundary_width, sprite.world_width);
        max_boundary_width = std::max(max_boundary_width, sprite.world_width);
        min_boundary_height = std::min(min_boundary_height, sprite.world_height);
        max_boundary_height = std::max(max_boundary_height, sprite.world_height);
    };

    auto depth_for_traversal = [&](const Assets::ActiveTraversalEntry& entry) -> double {
        if (!entry.asset) {
            return std::numeric_limits<double>::lowest();
        }
        if (std::isfinite(entry.depth_from_anchor)) {
            return entry.depth_from_anchor;
        }
        return render_depth::depth_from_anchor(anchor_depth,
                                               static_cast<double>(entry.asset->world_z()),
                                               entry.asset->render_depth_bias());
    };
    auto depth_for_boundary = [&](const DynamicBoundarySystem::BoundarySprite& sprite) -> double {
        return render_depth::depth_from_anchor(anchor_depth, static_cast<double>(sprite.world_z));
    };

    const auto& active_traversal = assets_->active_traversal();
    std::vector<Asset*> rendered_assets_for_debug;
    rendered_assets_for_debug.reserve(active_traversal.size());

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

            rendered_assets_for_debug.push_back(asset);
            composite_renderer_.update(asset, 0.0f);
            const world::GridPoint* gp = traversal_entry.grid_point ? traversal_entry.grid_point
                                                                    : cam.grid_point_for_asset(asset);
            const float perspective_scale =
                (gp && std::isfinite(gp->perspective_scale) && gp->perspective_scale > 0.0f)
                    ? gp->perspective_scale
                    : 1.0f;
            const float base_world_z = static_cast<float>(asset->world_z());
            const double depth_bias = asset->render_depth_bias();

            for (RenderObject& obj : asset->render_package) {
                if (!obj.texture) {
                    continue;
                }
                const float effective_world_z = base_world_z + obj.world_z_offset;
                WarpedMesh mesh{};
                if (!build_perspective_mesh(obj, cam, perspective_scale, effective_world_z, mesh)) {
                    continue;
                }
                if (mesh.vertices.size() != 4 || mesh.indices.size() != 6) {
                    continue;
                }
                SDL_Vertex verts[4];
                int indices[6];
                for (int i = 0; i < 4; ++i) verts[i] = mesh.vertices[static_cast<std::size_t>(i)];
                for (int i = 0; i < 6; ++i) indices[i] = mesh.indices[static_cast<std::size_t>(i)];

                const double draw_depth = render_depth::depth_from_anchor(anchor_depth,
                                                                          static_cast<double>(effective_world_z),
                                                                          depth_bias);
                geometry_batcher_->addQuad(obj.texture, verts, indices, obj.blend_mode, draw_depth);
            }
            continue;
        }

        if (boundary_index < boundary_sprites.size()) {
            const auto& sprite = boundary_sprites[boundary_index++];
            queue_boundary_sprite(sprite, next_boundary_depth);
            continue;
        }
    }

    ++s_boundary_render_debug_counter;
    if ((s_boundary_render_debug_counter % 120) == 0) {
        vibble::log::info(std::string{"[SceneRenderer] boundary draw stats: sprites="} +
                          std::to_string(boundary_sprites.size()) +
                          " queued=" + std::to_string(queued_boundary_sprites) +
                          " width=[" + std::to_string(min_boundary_width) + "," + std::to_string(max_boundary_width) + "]" +
                          " height=[" + std::to_string(min_boundary_height) + "," + std::to_string(max_boundary_height) + "]");
    }

    geometry_batcher_->flush();

    if (anchor_point_debug_enabled_) {
        render_anchor_debug_markers(renderer_, screen_width_, screen_height_, rendered_assets_for_debug);
    }

    // Dev grid overlay is projected against the final scene output.
    if (assets_->dev_grid_overlay_callback_) {
        assets_->dev_grid_overlay_callback_();
    }
}

void SceneRenderer::render_sky_layer(const WarpedScreenGrid& cam, bool depth_effects_enabled) {
    if (!depth_effects_enabled) {
        return;
    }
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    const auto floor_params = cam.compute_floor_depth_params();
    double horizon_y = std::numeric_limits<double>::quiet_NaN();
    if (floor_params.enabled && std::isfinite(floor_params.horizon_screen_y)) {
        // Match the same clamped horizon used for floor warping and depth effects.
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
