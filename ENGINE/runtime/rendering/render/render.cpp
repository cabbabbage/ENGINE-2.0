#include "rendering/render/render.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <SDL3_image/SDL_image.h>

#include "assets/Asset.hpp"
#include "assets/asset_library.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/dynamic_fog_system.hpp"
#include "rendering/render/dynamic_boundary_system.hpp"
#include "animation/animation_update.hpp"
#include "gameplay/world/tiling/grid_tile.hpp"
#include "assets/animation.hpp"
#include "assets/animation_frame.hpp"
#include "utils/log.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"

namespace {
bool enforce_trapezoid(std::array<SDL_FPoint, 4>& points);
}

void GridTileRenderer::render(SDL_Renderer* renderer) {
    if (!renderer || !assets_) return;
    render(renderer, assets_->getView(), assets_->world_grid());
}

void GridTileRenderer::render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid) {
    if (!renderer) return;

    const auto& chunks = grid.active_chunks();
    if (chunks.empty()) return;

    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    int indices[6] = {0, 1, 2, 0, 2, 3};

    for (const world::Chunk* chunk : chunks) {
        if (!chunk) continue;
        for (const auto& tile : chunk->tiles) {
            if (!tile.texture || tile.world_rect.w <= 0 || tile.world_rect.h <= 0) continue;

            SDL_Point world_tl{ tile.world_rect.x, tile.world_rect.y };
            SDL_Point world_tr{ tile.world_rect.x + tile.world_rect.w, tile.world_rect.y };
            SDL_Point world_br{ tile.world_rect.x + tile.world_rect.w, tile.world_rect.y + tile.world_rect.h };
            SDL_Point world_bl{ tile.world_rect.x, tile.world_rect.y + tile.world_rect.h };

            auto floor_project = [&](SDL_Point world_pos, SDL_FPoint& out) -> bool {
                SDL_FPoint screen{};
                if (!cam.project_world_point(SDL_FPoint{static_cast<float>(world_pos.x), static_cast<float>(world_pos.y)},
                                             0.0f,
                                             screen)) {
                    return false;
                }
                if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) {
                    return false;
                }
                screen.y = cam.warp_floor_screen_y(static_cast<float>(world_pos.y), screen.y);
                if (!std::isfinite(screen.y)) {
                    return false;
                }
                out = SDL_FPoint{std::floor(screen.x), std::floor(screen.y)};
                return true;
            };

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

            const float area_doubled =
                (screen_tr.x - screen_tl.x) * (screen_bl.y - screen_tl.y) - (screen_bl.x - screen_tl.x) * (screen_tr.y - screen_tl.y);
            if (std::fabs(area_doubled) < 1e-5f) {
                continue;
            }

            int tex_w_int = 0, tex_h_int = 0;
            float tex_wf = 0.0f;
            float tex_hf = 0.0f;
            if (!SDL_GetTextureSize(tile.texture, &tex_wf, &tex_hf)) {
                continue;
            }
            tex_w_int = static_cast<int>(std::lround(tex_wf));
            tex_h_int = static_cast<int>(std::lround(tex_hf));
            const float tex_w = static_cast<float>(tex_w_int);
            const float tex_h = static_cast<float>(tex_h_int);
            if (tex_w <= 0.0f || tex_h <= 0.0f) {
                continue;
            }
            const float padding_x = 0.5f / tex_w;
            const float padding_y = 0.5f / tex_h;

            const float tx0 = padding_x;
            const float ty0 = padding_y;
            const float tx1 = 1.0f - padding_x;
            const float ty1 = 1.0f - padding_y;

            SDL_Vertex vertices[4]{};
            vertices[0].position = SDL_FPoint{ screen_tl.x, screen_tl.y };
            vertices[1].position = SDL_FPoint{ screen_tr.x, screen_tr.y };
            vertices[2].position = SDL_FPoint{ screen_br.x, screen_br.y };
            vertices[3].position = SDL_FPoint{ screen_bl.x, screen_bl.y };
            vertices[0].color = vertices[1].color = vertices[2].color = vertices[3].color = white;
            vertices[0].tex_coord = SDL_FPoint{ tx0, ty0 };
            vertices[1].tex_coord = SDL_FPoint{ tx1, ty0 };
            vertices[2].tex_coord = SDL_FPoint{ tx1, ty1 };
            vertices[3].tex_coord = SDL_FPoint{ tx0, ty1 };

            SDL_RenderGeometry(renderer, tile.texture, vertices, 4, indices, 6);
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

bool enforce_trapezoid(std::array<SDL_FPoint, 4>& points) {
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

bool build_perspective_mesh(const RenderObject& obj,
                            const WarpedScreenGrid& cam,
                            float perspective_scale,
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

    // Render package dimensions already include the per-grid-point perspective scale;
    // strip it off so the projection step is the sole place that applies perspective.
    const float safe_perspective =
        (std::isfinite(perspective_scale) && perspective_scale > 0.0f)
            ? perspective_scale
            : 1.0f;
    const float world_width = static_cast<float>(rect.w) / safe_perspective;
    const float world_height = static_cast<float>(rect.h) / safe_perspective;
    const float half_width = world_width * 0.5f;
    const float height = world_height;
    const float base_z = obj.world_z_offset;
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
  sky_texture_path_(std::filesystem::path("resources") / "misc_content" / "sky.png"),
  floor_gradient_path_(std::filesystem::path("resources") / "misc_content" / "floor_gradient.png"),
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
    destroy_floor_gradient_texture();
    if (scene_composite_tex_) { SDL_DestroyTexture(scene_composite_tex_); scene_composite_tex_ = nullptr; }
    if (postprocess_tex_)     { SDL_DestroyTexture(postprocess_tex_);     postprocess_tex_     = nullptr; }
    if (blur_tex_)            { SDL_DestroyTexture(blur_tex_);            blur_tex_            = nullptr; }
}

SDL_Renderer* SceneRenderer::get_renderer() const {
    return renderer_;
}

void SceneRenderer::set_movement_debug_enabled(bool enabled) {
    debug_auto_paths_ = enabled;
}

void SceneRenderer::set_movement_debug_visible(bool visible) {
    movement_debug_visible_ = visible;
}

void SceneRenderer::invalidate_dynamic_boundary_system() {
    if (dynamic_boundary_system_) {
        dynamic_boundary_system_->invalidate_config();
    }
}

void SceneRenderer::render() {
    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    ++frame_counter_;

    WarpedScreenGrid& cam = assets_->getView();
    world::WorldGrid& grid = assets_->world_grid();

    const auto& render_traversal = assets_->active_traversal();

    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, map_clear_color_.r, map_clear_color_.g, map_clear_color_.b, map_clear_color_.a);
    SDL_RenderClear(renderer_);

    render_floor_gradient();

    const bool depth_effects_enabled = assets_->depth_effects_enabled();
    render_sky_layer(cam, depth_effects_enabled);

    if (tile_renderer_) {
        tile_renderer_->render(renderer_, cam, grid);
    }

    if (assets_->dev_grid_overlay_callback_) {
        assets_->dev_grid_overlay_callback_();
    }

    const float flicker_time_seconds = ticks_to_seconds(SDL_GetTicks64());
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    // Update fog system before rendering
    const bool should_render_fog = assets_->fog_visible();
    if (dynamic_fog_system_ && should_render_fog) {
        dynamic_fog_system_->update(cam, grid);
    }

    // Update boundary system before rendering (uses frame delta of 16.67ms as estimate)
    const bool boundary_assets_visible = assets_->boundary_assets_visible();
    const bool should_render_boundaries =
        boundary_assets_visible && dynamic_boundary_system_ && dynamic_boundary_system_->is_initialized();
    const float boundary_delta_ms = static_cast<float>(assets_->frame_delta_seconds() * 1000.0);
    if (should_render_boundaries) {
        dynamic_boundary_system_->update(cam, grid, assets_, boundary_delta_ms);
    }

    const double anchor_world_y = cam.anchor_world_y();
    auto depth_from_anchor = [&](double world_y) {
        return anchor_world_y - world_y;
    };

    // Sprites are depth-sorted inside their respective update() calls; bind directly, no copy.
    static const std::vector<DynamicFogSystem::FogSprite>      kEmptyFogSprites;
    static const std::vector<DynamicBoundarySystem::BoundarySprite> kEmptyBoundarySprites;
    const auto& fog_sprites      = (dynamic_fog_system_ && should_render_fog)
        ? dynamic_fog_system_->get_fog_sprites()         : kEmptyFogSprites;
    const auto& boundary_sprites = should_render_boundaries
        ? dynamic_boundary_system_->get_boundary_sprites() : kEmptyBoundarySprites;

    size_t fog_index = 0;
    size_t boundary_index = 0;
    const float fog_size_scale          = DynamicFogSystem::base_size_scale();
    const float fog_cull_margin         = 64.0f;
    const float fog_vertical_offset     = DynamicFogSystem::vertical_offset();
    const float boundary_cull_margin    = 64.0f;
    const float boundary_vertical_offset = DynamicBoundarySystem::vertical_offset();

    struct AssetRenderCandidate {
        Asset* asset = nullptr;
        world::GridPoint* grid_point = nullptr;
        double depth = 0.0;
    };

    size_t asset_index = 0;
    auto advance_asset_candidate = [&]() -> std::optional<AssetRenderCandidate> {
        while (asset_index < render_traversal.size()) {
            const auto& entry = render_traversal[asset_index++];
            Asset* asset = entry.asset;
            if (!asset || asset->is_hidden() || !asset->info) {
                continue;
            }
            if (const auto& tiling = asset->tiling_info(); tiling && tiling->is_valid()) {
                continue;
            }
            world::GridPoint* gp = entry.grid_point;
            if (!gp || !gp->on_screen) {
                continue;
            }
            return AssetRenderCandidate{asset, gp, entry.depth_from_anchor};
        }
        return std::nullopt;
    };

    auto next_asset = advance_asset_candidate();

    // Shared ground-sprite renderer used for both fog and boundary decorations.
    // world_width / world_height are the pre-scaled world-space dimensions to project.
    auto render_ground_sprite = [&](SDL_Texture* texture, float world_x, float world_y,
                                    float world_z, float world_width, float world_height,
                                    int tex_w, int tex_h, float vert_offset, float cull_margin) {
        if (!texture || tex_w <= 0 || tex_h <= 0 || world_width <= 0.0f || world_height <= 0.0f) {
            return;
        }
        const float half_width = world_width * 0.5f;
        const float height = world_height;

        SDL_FPoint base_screen{};
        if (!project_world_point(cam, world_x, world_y, world_z, base_screen)) {
            return;
        }

        const float adjusted_y = base_screen.y + vert_offset;
        if (base_screen.x + half_width < -cull_margin ||
            base_screen.x - half_width > static_cast<float>(screen_width_) + cull_margin ||
            adjusted_y < -cull_margin ||
            adjusted_y - height > static_cast<float>(screen_height_) + cull_margin) {
            return;
        }

        const float padding_x = 0.5f / static_cast<float>(tex_w);
        const float padding_y = 0.5f / static_cast<float>(tex_h);
        const float u0 = padding_x,  u1 = 1.0f - padding_x;
        const float v0 = padding_y,  v1 = 1.0f - padding_y;

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

        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, 255);
        SDL_RenderGeometry(renderer_, texture, vertices, 4, kQuadIndices, 6);
    };

    while (next_asset || fog_index < fog_sprites.size() || boundary_index < boundary_sprites.size()) {
        const double asset_depth = next_asset ? next_asset->depth : std::numeric_limits<double>::lowest();
        const double fog_depth = fog_index < fog_sprites.size()
            ? depth_from_anchor(static_cast<double>(fog_sprites[fog_index].world_pos.y))
            : std::numeric_limits<double>::lowest();
        const double boundary_depth = boundary_index < boundary_sprites.size()
            ? depth_from_anchor(static_cast<double>(boundary_sprites[boundary_index].world_pos.y))
            : std::numeric_limits<double>::lowest();

        // Determine which to render next (highest depth = furthest from camera = render first)
        const double max_depth = std::max({asset_depth, fog_depth, boundary_depth});

        if (next_asset && asset_depth == max_depth) {
            const AssetRenderCandidate candidate = *next_asset;

            composite_renderer_.update(candidate.asset, flicker_time_seconds);

            const float fade_alpha = std::clamp(candidate.grid_point->horizon_fade_alpha * candidate.grid_point->near_camera_fade_alpha, 0.0f, 1.0f);
            auto apply_fade_alpha = [&](SDL_Color color) {
                if (fade_alpha >= 0.999f) {
                    return color;
                }
                const int scaled = static_cast<int>(std::lround(static_cast<float>(color.a) * fade_alpha));
                color.a = static_cast<Uint8>(std::clamp(scaled, 0, 255));
                return color;
            };

            for (const RenderObject& obj : candidate.asset->render_package) {
                WarpedMesh mesh{};
                const float perspective_scale = candidate.grid_point
                    ? std::max(0.0001f, candidate.grid_point->perspective_scale)
                    : 1.0f;
                if (!build_perspective_mesh(obj, cam, perspective_scale, mesh)) {
                    continue;
                }

                const SDL_Color color_mod = apply_fade_alpha(obj.color_mod);
                SDL_SetTextureBlendMode(obj.texture, obj.blend_mode);
                SDL_SetTextureColorMod(obj.texture, color_mod.r, color_mod.g, color_mod.b);
                SDL_SetTextureAlphaMod(obj.texture, color_mod.a);

                SDL_RenderGeometry(renderer_,
                                   obj.texture,
                                   mesh.vertices.data(),
                                   static_cast<int>(mesh.vertices.size()),
                                   mesh.indices.data(),
                                   static_cast<int>(mesh.indices.size()));
            }

            next_asset = advance_asset_candidate();
        } else if (boundary_index < boundary_sprites.size() && boundary_depth == max_depth) {
            const auto& bs = boundary_sprites[boundary_index];
            render_ground_sprite(bs.texture, bs.world_pos.x, bs.world_pos.y, static_cast<float>(bs.world_z),
                                 bs.world_width, bs.world_height,
                                 bs.texture_w, bs.texture_h, boundary_vertical_offset, boundary_cull_margin);
            ++boundary_index;
        } else if (fog_index < fog_sprites.size()) {
            const auto& fs = fog_sprites[fog_index];
            render_ground_sprite(fs.texture, fs.world_pos.x, fs.world_pos.y, static_cast<float>(fs.world_z),
                                 static_cast<float>(fs.texture_w) * fog_size_scale * fs.scale,
                                 static_cast<float>(fs.texture_h) * fog_size_scale * fs.scale,
                                 fs.texture_w, fs.texture_h, fog_vertical_offset, fog_cull_margin);
            ++fog_index;
        } else {
            break;
        }
    }

    if (debug_auto_paths_ && movement_debug_visible_) {
        static const std::array<SDL_Color, 6> kPathColors{{
            SDL_Color{255, 99, 71, 255},
            SDL_Color{50, 205, 50, 255},
            SDL_Color{65, 105, 225, 255},
            SDL_Color{255, 215, 0, 255},
            SDL_Color{199, 21, 133, 255},
            SDL_Color{0, 206, 209, 255},
        }};

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        for (const auto& entry : render_traversal) {
            Asset* asset = entry.asset;
            if (!asset || asset->is_hidden() || !asset->info || !asset->anim_) {
                continue;
            }
            const Plan* plan = asset->anim_->current_plan();
            if (!plan || plan->sanitized_checkpoints.empty()) {
                continue;
            }

            SDL_SetRenderDrawColor(renderer_, 160, 32, 240, 160);
            if (asset->info) {
                for (const auto& [anim_id, anim] : asset->info->animations) {
                    const std::size_t paths = anim.movement_path_count();
                    for (std::size_t path_idx = 0; path_idx < paths; ++path_idx) {
                        const auto& path_frames = anim.movement_path(path_idx);
                        SDL_Point cursor = {asset->world_x(), asset->world_y()};
                        for (const AnimationFrame& frame : path_frames) {
                            SDL_Point next{ cursor.x + frame.dx, cursor.y + frame.dy };
                            SDL_FPoint screen_cur  = cam.map_to_screen(cursor);
                            SDL_FPoint screen_next = cam.map_to_screen(next);
                            SDL_RenderLine(renderer_, static_cast<int>(std::lround(screen_cur.x)), static_cast<int>(std::lround(screen_cur.y)), static_cast<int>(std::lround(screen_next.x)), static_cast<int>(std::lround(screen_next.y)));
                            SDL_Rect dot{
                                static_cast<int>(std::lround(screen_next.x)) - 2, static_cast<int>(std::lround(screen_next.y)) - 2, 4, 4 };
                            sdl_render::FillRect(renderer_, &dot);
                            cursor = next;
                        }
                    }
                }
            }

            if (!plan->strides.empty()) {
                SDL_SetRenderDrawColor(renderer_, 0, 0, 255, 160);
                SDL_Point cursor = plan->world_start;
                for (const auto& stride : plan->strides) {
                    auto it = asset->info->animations.find(stride.animation_id);
                    if (it != asset->info->animations.end()) {
                        const auto& anim = it->second;
                        const auto& path_frames = anim.movement_path(stride.path_index);
                        int count = std::min(static_cast<int>(path_frames.size()), stride.frames);
                        for (int i = 0; i < count; ++i) {
                            const AnimationFrame& frame = path_frames[i];
                            SDL_Point next{ cursor.x + frame.dx, cursor.y + frame.dy };
                            SDL_FPoint screen_cur = cam.map_to_screen(cursor);
                            SDL_FPoint screen_next = cam.map_to_screen(next);
                            SDL_RenderLine(renderer_, static_cast<int>(std::lround(screen_cur.x)), static_cast<int>(std::lround(screen_cur.y)), static_cast<int>(std::lround(screen_next.x)), static_cast<int>(std::lround(screen_next.y)));
                            cursor = next;
                        }
                    }
                }
            }

            if (!plan->sanitized_checkpoints.empty()) {
                const int visit_threshold = asset->anim_->visit_threshold_px();
                int threshold = visit_threshold;
                if (visit_threshold == 0) threshold = 32;
                const int segments = 24;
                std::vector<SDL_FPoint> ring;
                ring.reserve(static_cast<std::size_t>(segments) + 1);
                SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 180);
                for (std::size_t idx = 0; idx < plan->sanitized_checkpoints.size(); ++idx) {
                    const SDL_Point wp = plan->sanitized_checkpoints[idx];
                    ring.clear();
                    for (int i = 0; i <= segments; ++i) {
                        const double angle = (6.28318530717958647692 * static_cast<double>(i)) / static_cast<double>(segments);
                        SDL_Point pt{
                            wp.x + static_cast<int>(std::lround(static_cast<double>(threshold) * std::cos(angle))), wp.y + static_cast<int>(std::lround(static_cast<double>(threshold) * std::sin(angle))) };
                        ring.push_back(cam.map_to_screen(pt));
                    }
                    for (std::size_t i = 1; i < ring.size(); ++i) {
                        SDL_RenderLine(renderer_, static_cast<int>(std::lround(ring[i - 1].x)), static_cast<int>(std::lround(ring[i - 1].y)), static_cast<int>(std::lround(ring[i].x)), static_cast<int>(std::lround(ring[i].y)));
                    }
        }
    }

        }
    }
}

bool SceneRenderer::ensure_sky_texture() {
    if (sky_texture_ || sky_texture_failed_) {
        return sky_texture_ != nullptr;
    }
    if (!renderer_) {
        return false;
    }

    std::filesystem::path path = sky_texture_path_;
    if (!path.is_absolute()) {
        path = std::filesystem::current_path() / path;
    }

    const std::string path_str = path.string();
    SDL_Texture* tex = IMG_LoadTexture(renderer_, path_str.c_str());
    if (!tex) {
        vibble::log::warn(std::string{"[SceneRenderer] Failed to load sky texture '"} +
                         path_str + "': " + SDL_GetError());
        sky_texture_failed_ = true;
        return false;
    }

    int tex_w = 0;
    int tex_h = 0;
    float tex_wf = 0.0f;
    float tex_hf = 0.0f;
    if (!SDL_GetTextureSize(tex, &tex_wf, &tex_hf)) {
        vibble::log::warn(std::string{"[SceneRenderer] Invalid sky texture '"} +
                          path_str + "': " + SDL_GetError());
        SDL_DestroyTexture(tex);
        sky_texture_failed_ = true;
        return false;
    }
    tex_w = static_cast<int>(std::lround(tex_wf));
    tex_h = static_cast<int>(std::lround(tex_hf));
    if (tex_w <= 0 || tex_h <= 0) {
        vibble::log::warn(std::string{"[SceneRenderer] Invalid sky texture '"} +
                          path_str + "': " + SDL_GetError());
        SDL_DestroyTexture(tex);
        sky_texture_failed_ = true;
        return false;
    }

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    sky_texture_        = tex;
    sky_texture_width_  = tex_w;
    sky_texture_height_ = tex_h;
    return true;
}

void SceneRenderer::destroy_sky_texture() {
    if (sky_texture_) {
        SDL_DestroyTexture(sky_texture_);
        sky_texture_ = nullptr;
    }
    sky_texture_width_  = 0;
    sky_texture_height_ = 0;
}

void SceneRenderer::render_sky_layer(const WarpedScreenGrid& cam, bool depth_effects_enabled) {
    if (!depth_effects_enabled) {
        return;
    }
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    constexpr double kHalfFovY = 3.14159265358979323846 / 4.0;
    const double pitch_rad = cam.current_pitch_radians();
    const double tan_pitch = std::tan(pitch_rad);
    const double tan_half_fov_y = std::tan(kHalfFovY);
    const double horizon_ndc = (std::isfinite(tan_pitch) && std::isfinite(tan_half_fov_y) && tan_half_fov_y != 0.0)
        ? (tan_pitch / tan_half_fov_y)
        : 0.0;
    const double horizon_y =
        static_cast<double>(screen_height_) * 0.5 - horizon_ndc * static_cast<double>(screen_height_) * 0.5;
    if (!std::isfinite(horizon_y)) {
        return;
    }
    if (horizon_y < 0.0 || horizon_y > static_cast<double>(screen_height_)) {
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

    const float sky_visible_height =
        std::clamp(static_cast<float>(horizon_y), 0.0f, static_cast<float>(screen_height_));
    if (sky_visible_height <= 0.0f) {
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

bool SceneRenderer::ensure_floor_gradient_texture() {
    if (floor_gradient_texture_ || floor_gradient_failed_) {
        return floor_gradient_texture_ != nullptr;
    }
    if (!renderer_) {
        return false;
    }

    std::filesystem::path path = floor_gradient_path_;
    if (!path.is_absolute()) {
        path = std::filesystem::current_path() / path;
    }

    const std::string path_str = path.string();
    SDL_Texture* tex = IMG_LoadTexture(renderer_, path_str.c_str());
    if (!tex) {
        vibble::log::warn(std::string{"[SceneRenderer] Failed to load floor gradient texture '"} +
                         path_str + "': " + SDL_GetError());
        floor_gradient_failed_ = true;
        return false;
    }

    int tex_w = 0;
    int tex_h = 0;
    float tex_wf = 0.0f;
    float tex_hf = 0.0f;
    if (!SDL_GetTextureSize(tex, &tex_wf, &tex_hf)) {
        vibble::log::warn(std::string{"[SceneRenderer] Invalid floor gradient texture '"} +
                          path_str + "': " + SDL_GetError());
        SDL_DestroyTexture(tex);
        floor_gradient_failed_ = true;
        return false;
    }
    tex_w = static_cast<int>(std::lround(tex_wf));
    tex_h = static_cast<int>(std::lround(tex_hf));
    if (tex_w <= 0 || tex_h <= 0) {
        vibble::log::warn(std::string{"[SceneRenderer] Invalid floor gradient texture '"} +
                          path_str + "': " + SDL_GetError());
        SDL_DestroyTexture(tex);
        floor_gradient_failed_ = true;
        return false;
    }

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    floor_gradient_texture_  = tex;
    floor_gradient_width_    = tex_w;
    floor_gradient_height_   = tex_h;
    return true;
}

void SceneRenderer::destroy_floor_gradient_texture() {
    if (floor_gradient_texture_) {
        SDL_DestroyTexture(floor_gradient_texture_);
        floor_gradient_texture_ = nullptr;
    }
    floor_gradient_width_  = 0;
    floor_gradient_height_ = 0;
}

void SceneRenderer::render_floor_gradient() {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    if (!ensure_floor_gradient_texture() || !floor_gradient_texture_) {
        return;
    }

    const float tex_w = static_cast<float>(floor_gradient_width_);
    const float tex_h = static_cast<float>(floor_gradient_height_);
    if (tex_w <= 0.0f || tex_h <= 0.0f) {
        return;
    }

    const float scale = static_cast<float>(screen_width_) / tex_w;
    const float target_w = tex_w * scale;
    const float target_h = tex_h * scale;
    if (!std::isfinite(target_h) || target_h <= 0.0f || !std::isfinite(scale)) {
        return;
    }

    SDL_FRect dst{0.0f, 0.0f, target_w, target_h};

    SDL_SetTextureColorMod(floor_gradient_texture_, 255, 255, 255);
    SDL_SetTextureAlphaMod(floor_gradient_texture_, 255);
    sdl_render::Texture(renderer_, floor_gradient_texture_, nullptr, &dst);
}



