#include "rendering/render/floor_composer.hpp"

#include "core/AssetsManager.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/tiling/grid_tile.hpp"
#include "gameplay/world/world_grid.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/warped_screen_grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace {

constexpr float kQuadEpsilon = 1.0e-5f;

void destroy_texture(SDL_Texture*& texture) {
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
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

    out_screen.x = std::floor(out_screen.x);
    out_screen.y = std::floor(out_screen.y);
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
    if (cache.size() > 65536) {
        cache.clear();
    }
}

} // namespace

GridTileRenderer::GridTileRenderer(Assets* assets)
    : assets_(assets) {}

void GridTileRenderer::invalidate_texture_cache() {
    texture_size_cache_.clear();
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

    maybe_prune_ground_tile_projection_cache();
    auto& cache = ground_tile_projection_cache();
    const std::uint64_t camera_version = cam.camera_state_version();
    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    static constexpr int indices[6] = {0, 1, 2, 0, 2, 3};

    for (const world::Chunk* chunk : chunks) {
        if (!chunk) {
            continue;
        }

        for (const auto& tile : chunk->tiles) {
            if (!tile.texture || tile.world_rect.w <= 0 || tile.world_rect.h <= 0) {
                continue;
            }

            const void* tile_key = static_cast<const void*>(&tile);
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

                verts[0].position = points[0];
                verts[1].position = points[1];
                verts[2].position = points[2];
                verts[3].position = points[3];
                for (auto& v : verts) {
                    v.color = white;
                }
                verts[0].tex_coord = SDL_FPoint{0.0f, 0.0f};
                verts[1].tex_coord = SDL_FPoint{tex_size.x, 0.0f};
                verts[2].tex_coord = SDL_FPoint{tex_size.x, tex_size.y};
                verts[3].tex_coord = SDL_FPoint{0.0f, tex_size.y};

                auto& entry = cache[tile_key];
                entry.renderer = renderer;
                entry.texture = tile.texture;
                entry.world_rect = tile.world_rect;
                entry.camera_state_version = camera_version;
                entry.vertices[0] = verts[0];
                entry.vertices[1] = verts[1];
                entry.vertices[2] = verts[2];
                entry.vertices[3] = verts[3];
                entry.valid = true;
            }

            if (batcher) {
                batcher->addQuad(tile.texture, verts, indices, SDL_BLENDMODE_BLEND, 1000000.0);
            } else {
                SDL_RenderGeometry(renderer, tile.texture, verts, 4, indices, 6);
            }
        }
    }
}

FloorComposer::FloorComposer(SDL_Renderer* renderer, Assets* assets)
    : renderer_(renderer),
      tile_renderer_(assets) {}

FloorComposer::~FloorComposer() {
    destroy_owned_textures();
}

void FloorComposer::destroy_owned_textures() {
    destroy_texture(floor_base_texture_);
    destroy_texture(floor_light_mask_texture_);
    destroy_texture(floor_light_falloff_texture_);
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

    texture = SDL_CreateTexture(renderer_,
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
    SDL_SetRenderTarget(renderer_, texture);
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
    SDL_Texture* texture = SDL_CreateTexture(renderer_,
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
        SDL_DestroyTexture(texture);
        return nullptr;
    }

    auto* base = static_cast<std::uint8_t*>(pixels);
    const SDL_PixelFormatDetails* pixel_format = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA8888);
    if (!pixel_format) {
        SDL_UnlockTexture(texture);
        SDL_DestroyTexture(texture);
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

SDL_Texture* FloorComposer::compose(const WarpedScreenGrid& cam,
                                    const world::WorldGrid& grid,
                                    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
                                    bool runtime_lighting_enabled,
                                    double max_cull_depth,
                                    SDL_Color clear_color,
                                    bool render_floor_tiles) {
    if (!renderer_ || !ensure_sized_target(floor_base_texture_) || !ensure_sized_target(floor_light_mask_texture_)) {
        return nullptr;
    }

    const float horizon_y = compute_horizon_screen_y(cam);
    const int floor_top = std::clamp(static_cast<int>(std::floor(horizon_y)), 0, screen_height_);
    const int floor_height = std::max(0, screen_height_ - floor_top);
    SDL_Rect floor_clip{0, floor_top, screen_width_, floor_height};

    clear_target(floor_base_texture_);
    SDL_SetRenderTarget(renderer_, floor_base_texture_);
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

    if (!runtime_lighting_enabled) {
        return floor_base_texture_;
    }

    clear_target(floor_light_mask_texture_);
    SDL_SetRenderTarget(renderer_, floor_light_mask_texture_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    if (floor_height > 0) {
        SDL_SetRenderClipRect(renderer_, &floor_clip);
        SDL_SetRenderDrawColor(renderer_, 36, 38, 42, 255);
        const SDL_FRect full_rect{0.0f, 0.0f, static_cast<float>(screen_width_), static_cast<float>(screen_height_)};
        SDL_RenderFillRect(renderer_, &full_rect);

        const float floor_light_cull_depth = static_cast<float>(std::max(1.0, max_cull_depth * 0.5));
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
                    SDL_RenderTexture(renderer_, falloff_texture, nullptr, &dst_rect);
                }
            }

            SDL_SetTextureAlphaMod(falloff_texture, 255);
            SDL_SetTextureColorMod(falloff_texture, 255, 255, 255);
        }

        SDL_SetRenderClipRect(renderer_, nullptr);
    }

    SDL_SetRenderTarget(renderer_, floor_base_texture_);
    SDL_SetTextureBlendMode(floor_light_mask_texture_, SDL_BLENDMODE_MOD);
    SDL_SetTextureAlphaMod(floor_light_mask_texture_, 255);
    SDL_SetTextureColorMod(floor_light_mask_texture_, 255, 255, 255);
    SDL_RenderTexture(renderer_, floor_light_mask_texture_, nullptr, nullptr);

    return floor_base_texture_;
}
