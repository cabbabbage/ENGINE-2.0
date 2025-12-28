#include "render/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <array>
#include <iostream>

#include <SDL_image.h>

#include "asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "render/warped_screen_grid.hpp"
#include "animation_update/animation_update.hpp"
#include "tiling/grid_tile.hpp"
#include "asset/animation.hpp"
#include "asset/animation_frame.hpp"
#include "utils/log.hpp"
#include "world/chunk.hpp"
#include "world/world_grid.hpp"
#include "map_generation/map_layers_geometry.hpp"

void GridTileRenderer::render(SDL_Renderer* renderer) {
    if (!renderer || !assets_) return;
    render(renderer, assets_->getView(), assets_->world_grid());
}

void GridTileRenderer::render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid) {
    if (!renderer) return;

    const auto& chunks = grid.active_chunks();
    if (chunks.empty()) return;

    const SDL_Color white{255, 255, 255, 255};
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

            const float area_doubled =
                (screen_tr.x - screen_tl.x) * (screen_bl.y - screen_tl.y) - (screen_bl.x - screen_tl.x) * (screen_tr.y - screen_tl.y);
            if (std::fabs(area_doubled) < 1e-5f) {
                continue;
            }

            int tex_w_int = 0, tex_h_int = 0;
            if (SDL_QueryTexture(tile.texture, nullptr, nullptr, &tex_w_int, &tex_h_int) != 0) {
                continue;
            }
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

struct WarpedQuad {
    std::array<SDL_Vertex, 4> vertices{};
};

struct WorldCorner {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

bool build_warped_quad(const RenderObject& obj,
                       const WarpedScreenGrid& cam,
                       WarpedQuad& quad) {
    if (!obj.texture) {
        return false;
    }

    const SDL_Rect& rect = obj.screen_rect;
    if (rect.w <= 0 || rect.h <= 0) {
        return false;
    }

    const float anchor_x = static_cast<float>(rect.x);
    const float anchor_y = static_cast<float>(rect.y);
    const float half_w = static_cast<float>(rect.w) * 0.5f;
    const float height = static_cast<float>(rect.h);
    const float base_z = obj.world_z_offset;

    std::array<WorldCorner, 4> world = {{
        WorldCorner{anchor_x - half_w, anchor_y, base_z + height},
        WorldCorner{anchor_x + half_w, anchor_y, base_z + height},
        WorldCorner{anchor_x + half_w, anchor_y, base_z},
        WorldCorner{anchor_x - half_w, anchor_y, base_z}
    }};

    if (std::abs(obj.angle) > 0.001) {
        SDL_Point pivot = obj.use_custom_center ? obj.center : SDL_Point{rect.w / 2, rect.h / 2};
        const float pivot_x = anchor_x + static_cast<float>(pivot.x) - half_w;
        const float pivot_z = base_z + height - static_cast<float>(pivot.y);
        const float rad = static_cast<float>(obj.angle * (std::acos(-1.0) / 180.0));
        const float cos_a = std::cos(rad);
        const float sin_a = std::sin(rad);

        for (auto& corner : world) {
            const float dx = corner.x - pivot_x;
            const float dz = corner.z - pivot_z;
            corner.x = pivot_x + dx * cos_a - dz * sin_a;
            corner.z = pivot_z + dx * sin_a + dz * cos_a;
        }
    }

    std::array<SDL_FPoint, 4> projected{};
    for (size_t i = 0; i < world.size(); ++i) {
        SDL_FPoint screen{};
        if (!cam.project_world_point(SDL_FPoint{world[i].x, world[i].y}, world[i].z, screen)) {
            return false;
        }
        projected[i] = screen;
    }

    int tex_w = obj.texture_w;
    int tex_h = obj.texture_h;
    if (!obj.has_texture_size) {
        if (SDL_QueryTexture(obj.texture, nullptr, nullptr, &tex_w, &tex_h) != 0 || tex_w <= 0 || tex_h <= 0) {
            return false;
        }
    }
    if (tex_w <= 0 || tex_h <= 0) {
        return false;
    }

    const float padding_x = 0.5f / static_cast<float>(tex_w);
    const float padding_y = 0.5f / static_cast<float>(tex_h);
    float u0 = padding_x;
    float u1 = 1.0f - padding_x;
    float v0 = padding_y;
    float v1 = 1.0f - padding_y;

    if ((obj.flip & SDL_FLIP_HORIZONTAL) != 0) {
        std::swap(u0, u1);
    }
    if ((obj.flip & SDL_FLIP_VERTICAL) != 0) {
        std::swap(v0, v1);
    }

    const SDL_Color white{255, 255, 255, 255};
    quad.vertices[0].position = projected[0];
    quad.vertices[1].position = projected[1];
    quad.vertices[2].position = projected[2];
    quad.vertices[3].position = projected[3];
    for (auto& vertex : quad.vertices) {
        vertex.color = white;
    }
    quad.vertices[0].tex_coord = SDL_FPoint{u0, v0};
    quad.vertices[1].tex_coord = SDL_FPoint{u1, v0};
    quad.vertices[2].tex_coord = SDL_FPoint{u1, v1};
    quad.vertices[3].tex_coord = SDL_FPoint{u0, v1};

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
  sky_texture_path_(std::filesystem::path("SRC") / "misc_content" / "sky.png"),
  composite_renderer_(renderer, assets)
{

    bool color_set = false;
    if (map_manifest.contains("maps") && map_manifest["maps"].contains(map_id)) {
        const auto& map_data = map_manifest["maps"][map_id];
        if (map_data.contains("map_light_data") && map_data["map_light_data"].is_object()) {
            const auto& mld = map_data["map_light_data"];
            if (mld.contains("map_color")) {
                const auto& mc = mld["map_color"];
                if (mc.contains("r") && mc["r"].contains("max") && mc["r"]["max"].is_number_integer() &&
                    mc.contains("g") && mc["g"].contains("max") && mc["g"]["max"].is_number_integer() &&
                    mc.contains("b") && mc["b"].contains("max") && mc["b"]["max"].is_number_integer() &&
                    mc.contains("a") && mc["a"].contains("max") && mc["a"]["max"].is_number_integer()) {
                    int r_max = mc["r"]["max"];
                    int g_max = mc["g"]["max"];
                    int b_max = mc["b"]["max"];
                    int a_max = mc["a"]["max"];
                    if (r_max >= 0 && r_max <= 255 && g_max >= 0 && g_max <= 255 &&
                        b_max >= 0 && b_max <= 255 && a_max >= 0 && a_max <= 255) {
                        map_clear_color_ = SDL_Color{static_cast<Uint8>(r_max), static_cast<Uint8>(g_max), static_cast<Uint8>(b_max), static_cast<Uint8>(a_max)};
                        color_set = true;
                    }
                }
            }
            if (mld.contains("intensity") && mld["intensity"].is_number()) {
                const int intensity_raw = static_cast<int>(mld["intensity"]);
                const int clamped       = std::clamp(intensity_raw, 0, 255);
                map_light_opacity_      = static_cast<float>(clamped) / 255.0f;
                if (!std::isfinite(map_light_opacity_)) {
                    map_light_opacity_ = SceneRenderer::kDefaultMapLightOpacity;
                }
            }
        }
    }
    if (!color_set) {

        map_clear_color_ = SDL_Color{69, 101, 74, 255};
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

    ensure_fog_texture();
}

SceneRenderer::~SceneRenderer() {
    destroy_darkness_overlay();
    destroy_sky_texture();
    destroy_fog_texture();
    if (scene_composite_tex_) { SDL_DestroyTexture(scene_composite_tex_); scene_composite_tex_ = nullptr; }
    if (postprocess_tex_)     { SDL_DestroyTexture(postprocess_tex_);     postprocess_tex_     = nullptr; }
    if (blur_tex_)            { SDL_DestroyTexture(blur_tex_);            blur_tex_            = nullptr; }
}

SDL_Renderer* SceneRenderer::get_renderer() const {
    return renderer_;
}

void SceneRenderer::set_dark_mask_enabled(bool enabled) {
    if (dark_mask_enabled_ == enabled) {
        return;
    }
    dark_mask_enabled_ = enabled;
    if (!dark_mask_enabled_) {
        destroy_darkness_overlay();
    }
}

void SceneRenderer::render() {
    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    ++frame_counter_;

    WarpedScreenGrid& cam = assets_->getView();
    world::WorldGrid& grid = assets_->world_grid();
    cam.rebuild_grid(grid, assets_->frame_delta_seconds());
    assets_->rebuild_active_from_screen_grid();

    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, map_clear_color_.r, map_clear_color_.g, map_clear_color_.b, map_clear_color_.a);
    SDL_RenderClear(renderer_);

    const bool depth_effects_enabled = assets_->depth_effects_enabled();
    render_sky_layer(cam, depth_effects_enabled);

    if (tile_renderer_) {
        tile_renderer_->render(renderer_, cam, grid);
    }

    const float flicker_time_seconds = ticks_to_seconds(SDL_GetTicks64());
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    const auto& active_assets = assets_->getActive();
    std::vector<Asset*> sorted_assets(active_assets.begin(), active_assets.end());
    std::sort(sorted_assets.begin(), sorted_assets.end(), [&](Asset* a, Asset* b) {
        world::GridPoint* ga = cam.grid_point_for_asset(a);
        world::GridPoint* gb = cam.grid_point_for_asset(b);
        if (!ga || !gb) return ga > gb;
        return ga->distance_to_camera > gb->distance_to_camera;
    });
    std::vector<DarkMaskSprite> dark_mask_sprites;
    dark_mask_sprites.reserve(std::max<std::size_t>(sorted_assets.size(), 8u));
    for (Asset* asset : sorted_assets) {
        if (!asset || asset->is_hidden() || !asset->info) {
            continue;
        }

        if (const auto& tiling = asset->tiling_info(); tiling && tiling->is_valid()) {

            continue;
        }

        world::GridPoint* gp = cam.grid_point_for_asset(asset);
        if (!gp || !gp->on_screen) {
            continue;
        }

        composite_renderer_.update(asset, gp, flicker_time_seconds);

        if (dark_mask_enabled_ && !asset->scene_mask_lights.empty()) {
            for (const RenderObject& mask_obj : asset->scene_mask_lights) {
                WarpedQuad quad{};
                if (!build_warped_quad(mask_obj, cam, quad)) {
                    continue;
                }
                DarkMaskSprite sprite;
                sprite.texture   = mask_obj.texture;
                sprite.vertices  = quad.vertices;
                sprite.color_mod = mask_obj.color_mod;
                dark_mask_sprites.push_back(sprite);
            }
        }

        for (const RenderObject& obj : asset->render_package) {
            WarpedQuad quad{};
            if (!build_warped_quad(obj, cam, quad)) {
                continue;
            }

            SDL_SetTextureBlendMode(obj.texture, obj.blend_mode);

            SDL_SetTextureColorMod(obj.texture, obj.color_mod.r, obj.color_mod.g, obj.color_mod.b);
            SDL_SetTextureAlphaMod(obj.texture, obj.color_mod.a);

            SDL_RenderGeometry(renderer_, obj.texture, quad.vertices.data(), 4, kQuadIndices, 6);
        }
    }

    render_fog_layer(cam, grid, depth_effects_enabled);

    if (dark_mask_enabled_) {
        render_dynamic_darkness_overlay(map_light_opacity_, dark_mask_sprites);
    }

    if (debug_auto_paths_) {
        static const std::array<SDL_Color, 6> kPathColors{{
            SDL_Color{255, 99, 71, 255},
            SDL_Color{50, 205, 50, 255},
            SDL_Color{65, 105, 225, 255},
            SDL_Color{255, 215, 0, 255},
            SDL_Color{199, 21, 133, 255},
            SDL_Color{0, 206, 209, 255},
        }};

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        for (Asset* asset : sorted_assets) {
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
                        SDL_Point cursor = asset->pos;
                        for (const AnimationFrame& frame : path_frames) {
                            SDL_Point next{ cursor.x + frame.dx, cursor.y + frame.dy };
                            SDL_FPoint screen_cur  = cam.map_to_screen(cursor);
                            SDL_FPoint screen_next = cam.map_to_screen(next);
                            SDL_RenderDrawLine(renderer_, static_cast<int>(std::lround(screen_cur.x)), static_cast<int>(std::lround(screen_cur.y)), static_cast<int>(std::lround(screen_next.x)), static_cast<int>(std::lround(screen_next.y)));
                            SDL_Rect dot{
                                static_cast<int>(std::lround(screen_next.x)) - 2, static_cast<int>(std::lround(screen_next.y)) - 2, 4, 4 };
                            SDL_RenderFillRect(renderer_, &dot);
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
                            SDL_RenderDrawLine(renderer_, static_cast<int>(std::lround(screen_cur.x)), static_cast<int>(std::lround(screen_cur.y)), static_cast<int>(std::lround(screen_next.x)), static_cast<int>(std::lround(screen_next.y)));
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
                        SDL_RenderDrawLine(renderer_, static_cast<int>(std::lround(ring[i - 1].x)), static_cast<int>(std::lround(ring[i - 1].y)), static_cast<int>(std::lround(ring[i].x)), static_cast<int>(std::lround(ring[i].y)));
                    }
        }
    }

        }
    }
}

bool SceneRenderer::ensure_darkness_overlay() {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }
    if (darkness_overlay_allocation_failed_) {
        return false;
    }

    if (darkness_overlay_texture_ &&
        (darkness_overlay_width_ != screen_width_ || darkness_overlay_height_ != screen_height_)) {
        destroy_darkness_overlay();
    }

    if (!darkness_overlay_texture_) {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_width_, screen_height_);
        if (!texture) {
            vibble::log::warn(std::string{"[SceneRenderer] Failed to allocate darkness overlay: "} + SDL_GetError());
            darkness_overlay_allocation_failed_ = true;
            return false;
        }
        darkness_overlay_texture_ = texture;
        darkness_overlay_width_   = screen_width_;
        darkness_overlay_height_  = screen_height_;
        SDL_SetTextureBlendMode(darkness_overlay_texture_, SDL_BLENDMODE_BLEND);
        darkness_overlay_allocation_failed_ = false;
    }

    return darkness_overlay_texture_ != nullptr;
}

void SceneRenderer::destroy_darkness_overlay() {
    if (darkness_overlay_texture_) {
        SDL_DestroyTexture(darkness_overlay_texture_);
        darkness_overlay_texture_ = nullptr;
        darkness_overlay_width_   = 0;
        darkness_overlay_height_  = 0;
    }
    darkness_overlay_allocation_failed_ = false;
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
                          path_str + "': " + IMG_GetError());
        sky_texture_failed_ = true;
        return false;
    }

    int tex_w = 0;
    int tex_h = 0;
    if (SDL_QueryTexture(tex, nullptr, nullptr, &tex_w, &tex_h) != 0 || tex_w <= 0 || tex_h <= 0) {
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
    SDL_RenderCopyF(renderer_, sky_texture_, nullptr, &dst);
}

bool SceneRenderer::ensure_fog_texture() {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }
    if (fog_texture_failed_) {
        return false;
    }
    if (!fog_textures_.empty()) {
        return true;
    }

    const double desired_span = (map_radius_world_ > 0.0)
        ? std::clamp(map_radius_world_ * 2.0, 0.0, 50000.0)
        : static_cast<double>(screen_width_ * 2);
    fog_span_width_px_ = std::max(screen_width_, static_cast<int>(std::lround(desired_span)));
    fog_texture_width_  = std::clamp(fog_span_width_px_ / 4, 512, 2048);
    fog_texture_height_ = std::clamp(screen_height_ / 3, 128, 640);

    auto make_layer = [&](int layer_index) -> SDL_Texture* {
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, fog_texture_width_, fog_texture_height_, 32, SDL_PIXELFORMAT_RGBA8888);
        if (!surf) {
            return nullptr;
        }

        auto* pixels = static_cast<Uint32*>(surf->pixels);
        const int pitch = surf->pitch / static_cast<int>(sizeof(Uint32));
        const float layer_softness = 0.65f + 0.05f * static_cast<float>(layer_index);

        for (int y = 0; y < fog_texture_height_; ++y) {
            const float y_norm = (fog_texture_height_ > 1) ? static_cast<float>(y) / static_cast<float>(fog_texture_height_ - 1) : 0.0f;
            const float fade_in  = smoothstep(0.02f, 0.15f + layer_softness * 0.05f, y_norm);
            const float fade_out = 1.0f - smoothstep(0.72f - layer_softness * 0.05f, 0.98f, y_norm);
            const float vertical = std::clamp(fade_in * fade_out, 0.0f, 1.0f);

            for (int x = 0; x < fog_texture_width_; ++x) {
                const float x_norm = (fog_texture_width_ > 1) ? static_cast<float>(x) / static_cast<float>(fog_texture_width_ - 1) : 0.0f;
                float horizontal = 1.0f - std::abs(1.0f - 2.0f * x_norm);
                horizontal = std::clamp(horizontal, 0.0f, 1.0f);
                horizontal = horizontal * horizontal;

                const float alpha_f = std::clamp(horizontal * vertical * (0.9f + 0.05f * layer_index), 0.0f, 1.0f);
                const Uint8 a = static_cast<Uint8>(std::clamp(std::lround(alpha_f * 255.0f), 0L, 255L));
                pixels[y * pitch + x] = SDL_MapRGBA(surf->format, 255, 255, 255, a);
            }
        }

        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
        SDL_FreeSurface(surf);
        if (!tex) {
            return nullptr;
        }
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeBest);
#endif
        return tex;
    };

    fog_textures_.reserve(5);
    for (int i = 0; i < 5; ++i) {
        SDL_Texture* tex = make_layer(i);
        if (!tex) {
            destroy_fog_texture();
            fog_texture_failed_ = true;
            return false;
        }
        fog_textures_.push_back(tex);
    }

    return true;
}

void SceneRenderer::destroy_fog_texture() {
    for (SDL_Texture* tex : fog_textures_) {
        if (tex) {
            SDL_DestroyTexture(tex);
        }
    }
    fog_textures_.clear();
    fog_texture_width_  = 0;
    fog_texture_height_ = 0;
    fog_span_width_px_  = 0;
    fog_texture_failed_ = false;
}

void SceneRenderer::render_fog_layer(const WarpedScreenGrid& cam, const world::WorldGrid& grid, bool depth_effects_enabled) {
    if (!depth_effects_enabled) {
        return;
    }
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }
    (void)grid;
    if (!ensure_fog_texture() || fog_textures_.empty()) {
        return;
    }

    const auto params = cam.compute_floor_depth_params();
    if (!params.enabled || !std::isfinite(params.horizon_screen_y)) {
        return;
    }

    const float horizon_y = static_cast<float>(params.horizon_screen_y);
    if (!std::isfinite(horizon_y) ||
        horizon_y < -static_cast<float>(screen_height_) ||
        horizon_y > static_cast<float>(screen_height_)) {
        return;
    }

    const float bottom         = static_cast<float>(screen_height_);
    const float visible_height = std::max(1.0f, bottom - horizon_y);
    const float span_width     = static_cast<float>(std::max(fog_span_width_px_, screen_width_));
    const float center_x       = static_cast<float>(screen_width_) * 0.5f;
    const float pitch_factor   = std::clamp(static_cast<float>(params.pitch_norm), 0.0f, 1.0f);
    const float perspective_boost = 0.7f + 0.6f * pitch_factor;

    const std::size_t layer_count = fog_textures_.size();
    for (std::size_t idx = 0; idx < layer_count; ++idx) {
        SDL_Texture* tex = fog_textures_[idx];
        if (!tex) {
            continue;
        }

        const float depth_t = (static_cast<float>(idx) + 1.0f) / static_cast<float>(layer_count + 1);
        const float eased_depth = std::pow(depth_t, 0.85f);
        const float band_bottom = horizon_y + visible_height * eased_depth;
        const float band_height = std::clamp(visible_height * (0.12f + 0.06f * (1.0f - depth_t)), 4.0f, visible_height);

        SDL_FRect dst{
            center_x - span_width * 0.5f,
            band_bottom - band_height,
            span_width,
            band_height
        };

        float density = 0.22f + 0.55f * (1.0f - depth_t);
        density *= perspective_boost;
        const float distance_mix = 1.0f - std::clamp((band_bottom - horizon_y) / visible_height, 0.0f, 1.0f);
        density *= distance_mix;
        density = std::clamp(density, 0.0f, 1.0f);

        SDL_SetTextureColorMod(tex, 255, 255, 255);
        SDL_SetTextureAlphaMod(tex, static_cast<Uint8>(std::clamp(std::lround(density * 255.0f), 0L, 255L)));
        SDL_RenderCopyF(renderer_, tex, nullptr, &dst);
    }
}

void SceneRenderer::render_dynamic_darkness_overlay(float map_light_opacity,
                                                    const std::vector<DarkMaskSprite>& sprites) {
    if (!renderer_) {
        return;
    }

    const float overlay_alpha = std::clamp(map_light_opacity, 0.0f, 1.0f);
    if (overlay_alpha <= 0.0f) {
        ++darkness_overlay_skipped_frames_;
        darkness_overlay_skip_logged_ = true;
        return;
    }

    if (!ensure_darkness_overlay()) {
        ++darkness_overlay_skipped_frames_;
        darkness_overlay_skip_logged_ = true;
        return;
    }

    ++darkness_overlay_rendered_frames_;
    darkness_overlay_skip_logged_ = false;

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    SDL_SetRenderTarget(renderer_, darkness_overlay_texture_);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    const Uint8 overlay_alpha_byte = static_cast<Uint8>(std::clamp(std::lround(overlay_alpha * 255.0f), 0L, 255L));
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, overlay_alpha_byte);
    SDL_RenderClear(renderer_);

    if (!sprites.empty()) {
        SDL_BlendMode carve_mode = SDL_ComposeCustomBlendMode( SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);

        static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
        for (const DarkMaskSprite& sprite : sprites) {
            if (!sprite.texture) {
                continue;
            }
            SDL_SetTextureBlendMode(sprite.texture, carve_mode);
            SDL_SetTextureColorMod(sprite.texture, sprite.color_mod.r, sprite.color_mod.g, sprite.color_mod.b);
            SDL_SetTextureAlphaMod(sprite.texture, sprite.color_mod.a);
            SDL_RenderGeometry(renderer_, sprite.texture, sprite.vertices.data(), 4, kQuadIndices, 6);
        }
    }

    SDL_SetRenderTarget(renderer_, previous_target);

    SDL_SetTextureBlendMode(darkness_overlay_texture_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(darkness_overlay_texture_, overlay_alpha_byte);
    SDL_SetTextureColorMod(darkness_overlay_texture_, 0, 0, 0);

    SDL_Rect screen_dst{0, 0, screen_width_, screen_height_};
    SDL_RenderCopy(renderer_, darkness_overlay_texture_, nullptr, &screen_dst);
}
