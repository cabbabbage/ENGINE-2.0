#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "rendering/render/composite_asset_renderer.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "rendering/render/dynamic_fog_system.hpp"
#include "rendering/render/dynamic_boundary_system.hpp"
#include <SDL.h>

#include <nlohmann/json.hpp>

class Assets;
class WarpedScreenGrid;
class AssetLibrary;
namespace world { class WorldGrid; }

class GridTileRenderer {
public:
    explicit GridTileRenderer(Assets* assets) : assets_(assets) {}

    void render(SDL_Renderer* renderer);

    void render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid);

private:
    Assets* assets_ = nullptr;
};

class SceneRenderer {
public:
    SceneRenderer(SDL_Renderer* renderer, Assets* assets, int screen_width, int screen_height, const nlohmann::json& map_manifest, const std::string& map_id);
    ~SceneRenderer();

    void invalidate_dynamic_boundary_system();

    static inline bool prerequisites_ready(SDL_Renderer* renderer, Assets* assets, std::string* reason = nullptr) {
        if (!renderer) {
            if (reason) { *reason = "SDL_Renderer pointer is null."; }
            return false;
        }
        if (!assets) {
            if (reason) { *reason = "Assets pointer is null."; }
            return false;
        }
        if (reason) { reason->clear(); }
        return true;
    }

    void render();
    SDL_Renderer* get_renderer() const;

    void set_movement_debug_enabled(bool enabled);
    bool movement_debug_enabled() const { return debug_auto_paths_; }
    void set_movement_debug_visible(bool visible);
    bool movement_debug_visible() const { return movement_debug_visible_; }
    void set_map_clear_color(SDL_Color color) { map_clear_color_ = color; }
    SDL_Color map_clear_color() const { return map_clear_color_; }

private:
    struct PrevalidatedTag {};

    SceneRenderer(PrevalidatedTag, SDL_Renderer* renderer, Assets* assets, int screen_width, int screen_height, const nlohmann::json& map_manifest, const std::string& map_id);
    static PrevalidatedTag require_prerequisites(SDL_Renderer* renderer, Assets* assets);

    bool ensure_sky_texture();
    void destroy_sky_texture();
    void render_sky_layer(const WarpedScreenGrid& cam, bool depth_effects_enabled);

    bool ensure_floor_gradient_texture();
    void destroy_floor_gradient_texture();
    void render_floor_gradient();

    SDL_Renderer*  renderer_;
    Assets*        assets_;
    int            screen_width_;
    int            screen_height_;

    std::unique_ptr<GridTileRenderer> tile_renderer_;

    bool           debugging = false;
    bool           low_quality_rendering_ = false;

    std::uint64_t frame_counter_ = 0;

    SDL_Color    map_clear_color_{0, 128, 0, 255};
    bool         debug_auto_paths_ = true;
    bool         movement_debug_visible_ = true;

    CompositeAssetRenderer composite_renderer_;
    std::unique_ptr<DynamicFogSystem> dynamic_fog_system_;
    std::unique_ptr<DynamicBoundarySystem> dynamic_boundary_system_;

    std::uint32_t depthcue_warmup_frames_ = 8;

    SDL_Texture* scene_composite_tex_ = nullptr;
    SDL_Texture* postprocess_tex_     = nullptr;
    SDL_Texture* blur_tex_            = nullptr;
    std::filesystem::path sky_texture_path_;
    double                map_radius_world_ = 0.0;
    SDL_Texture*          sky_texture_       = nullptr;
    int                   sky_texture_width_ = 0;
    int                   sky_texture_height_ = 0;
    bool                  sky_texture_failed_ = false;

    std::filesystem::path floor_gradient_path_;
    SDL_Texture*          floor_gradient_texture_       = nullptr;
    int                   floor_gradient_width_ = 0;
    int                   floor_gradient_height_ = 0;
    bool                  floor_gradient_failed_ = false;
};
