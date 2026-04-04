#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rendering/render/composite_asset_renderer.hpp"
#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "rendering/render/dynamic_boundary_system.hpp"
#include <SDL3/SDL.h>

#include <nlohmann/json.hpp>

class Assets;
class WarpedScreenGrid;
class AssetLibrary;
namespace world { class WorldGrid; }
// Geometry batching system for reducing draw calls
class GeometryBatcher {
public:
    explicit GeometryBatcher(SDL_Renderer* renderer);

    struct DrawItem {
        SDL_Texture* texture = nullptr;
        SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
        SDL_Vertex vertices[4];
        double depth = 0.0;
    };

    // Add a quad to the batch with depth for sorting
    void addQuad(SDL_Texture* texture, const SDL_Vertex vertices[4], const int indices[6],
                 SDL_BlendMode blend_mode, double depth);

    // Flush all batches to the renderer
    void flush();

    // Clear all batches (call at start of frame)
    void clear();
    void for_each_item_far_to_near(const std::function<void(const DrawItem&)>& fn) const;

    // Get statistics for profiling
    size_t getDrawCallCount() const { return draw_call_count_; }
    size_t getTotalVertices() const { return total_vertices_; }
    double getLastFlushCpuMs() const { return last_flush_cpu_ms_; }

private:
    struct DepthBucket {
        std::vector<DrawItem> items;
    };

    SDL_Renderer* renderer_;
    std::map<std::int64_t, DepthBucket> depth_buckets_;
    DepthBucket invalid_depth_bucket_;
    size_t draw_call_count_ = 0;
    size_t total_vertices_ = 0;
    double last_flush_cpu_ms_ = 0.0;

    // Reusable buffers to avoid allocations
    std::vector<SDL_Vertex> vertex_buffer_;
    std::vector<int> index_buffer_;
};

class GridTileRenderer {
public:
    explicit GridTileRenderer(Assets* assets) : assets_(assets) {}

    void render(SDL_Renderer* renderer);

    void render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid);

    void render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid, GeometryBatcher* batcher);

    // Call when tile textures are rebuilt or assets are reloaded
    void invalidate_texture_cache();

private:
    bool fetch_texture_size(SDL_Texture* texture, SDL_FPoint& out_size);

    Assets* assets_ = nullptr;
    std::unordered_map<SDL_Texture*, SDL_FPoint> texture_size_cache_;
};

class SceneRenderer {
public:
    SceneRenderer(SDL_Renderer* renderer, Assets* assets, int screen_width, int screen_height, const nlohmann::json& map_manifest, const std::string& map_id);
    ~SceneRenderer();

    void invalidate_dynamic_boundary_system();
    const std::vector<DynamicBoundarySystem::BoundarySprite>& dynamic_boundary_sprites() const;

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
    void set_output_dimensions(int screen_width, int screen_height);
    int output_width() const { return screen_width_; }
    int output_height() const { return screen_height_; }
    std::optional<SDL_Point> postprocess_target_size() const;

    void set_movement_debug_enabled(bool enabled);
    bool movement_debug_enabled() const { return debug_auto_paths_; }
    void set_movement_debug_visible(bool visible);
    bool movement_debug_visible() const { return movement_debug_visible_; }
    void set_anchor_point_debug_enabled(bool enabled);
    bool anchor_point_debug_enabled() const { return anchor_point_debug_enabled_; }
    void set_map_clear_color(SDL_Color color) {
        map_clear_color_ = color;
        if (!sky_texture_) {
            fog_tint_color_ = color;
        }
    }
    SDL_Color map_clear_color() const { return map_clear_color_; }

private:
    struct MovementDebugPathSnapshot {
        std::vector<SDL_Point> world_points;
        SDL_Color color{48, 200, 255, 220};
    };

    struct MovementDebugAssetSnapshot {
        std::vector<MovementDebugPathSnapshot> paths;
    };

    struct MovementDebugObservedState {
        std::string animation_id;
        const class AnimationFrame* frame = nullptr;
        bool frame_is_first = false;
        bool frame_is_last = false;
    };

    struct PrevalidatedTag {};

    SceneRenderer(PrevalidatedTag, SDL_Renderer* renderer, Assets* assets, int screen_width, int screen_height, const nlohmann::json& map_manifest, const std::string& map_id);
    static PrevalidatedTag require_prerequisites(SDL_Renderer* renderer, Assets* assets);

    bool ensure_sky_texture();
    void destroy_sky_texture();
    void update_fog_tint_from_sky(SDL_Texture* sky_texture);
    void render_sky_layer(const WarpedScreenGrid& cam);
    void refresh_movement_debug_snapshots(const std::vector<Asset*>& visible_assets);
    void render_movement_debug_snapshots(const WarpedScreenGrid& cam,
                                         int screen_width,
                                         int screen_height,
                                         const std::vector<Asset*>& visible_assets) const;

    SDL_Renderer*  renderer_;
    Assets*        assets_;
    int            screen_width_;
    int            screen_height_;

    std::unique_ptr<GridTileRenderer> tile_renderer_;
    std::unique_ptr<GeometryBatcher> geometry_batcher_;

    bool           debugging = false;
    bool           low_quality_rendering_ = false;

    std::uint64_t frame_counter_ = 0;

    SDL_Color    map_clear_color_{0, 128, 0, 255};
    bool         debug_auto_paths_ = true;
    bool         movement_debug_visible_ = true;
    bool         anchor_point_debug_enabled_ = false;
    std::unordered_map<const Asset*, MovementDebugAssetSnapshot> movement_debug_snapshots_;
    std::unordered_map<const Asset*, MovementDebugObservedState> movement_debug_observed_state_;

    CompositeAssetRenderer composite_renderer_;
    LayerEffectProcessor layer_effect_processor_;
    std::unique_ptr<DynamicBoundarySystem> dynamic_boundary_system_;

    SDL_Texture* scene_composite_tex_ = nullptr;
    SDL_Texture* postprocess_tex_     = nullptr;
    SDL_Texture* blur_tex_            = nullptr;
    std::vector<SDL_Texture*> motion_blur_history_textures_;
    int motion_blur_history_write_index_ = 0;
    int motion_blur_valid_history_frames_ = 0;
    std::vector<SDL_Texture*> dof_layer_textures_;
    std::vector<SDL_Texture*> dof_blur_textures_;
    std::filesystem::path sky_texture_path_;
    double                map_radius_world_ = 0.0;
    SDL_Texture*          sky_texture_       = nullptr;
    int                   sky_texture_width_ = 0;
    int                   sky_texture_height_ = 0;
    bool                  sky_texture_failed_ = false;
    SDL_Color             fog_tint_color_{69, 101, 74, 255};
};
