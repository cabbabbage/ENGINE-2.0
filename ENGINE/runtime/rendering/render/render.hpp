#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
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

namespace render_internal {
struct FloorLightContact {
    float world_x = 0.0f;
    float world_z = 0.0f;
    float world_height = 0.0f;
    bool valid = false;
};

bool composite_dof_layers_to_gameplay_target(SDL_Renderer* renderer,
                                             SDL_Texture* gameplay_target,
                                             const std::vector<SDL_Texture*>& final_layer_textures,
                                             const std::vector<int>& non_empty_layers);
bool composite_scene_mid_layers(SDL_Renderer* renderer,
                                SDL_Texture* gameplay_target,
                                SDL_Texture* background_mid,
                                SDL_Texture* foreground_mid);
bool clear_gameplay_target_to_color(SDL_Renderer* renderer,
                                    SDL_Texture* gameplay_target,
                                    SDL_Color clear_color);
FloorLightContact resolve_floor_light_contact(float flat_world_x,
                                              float flat_world_z,
                                              float displaced_world_x,
                                              float displaced_world_z,
                                              float world_height);
bool project_floor_contact_to_screen(const WarpedScreenGrid& cam,
                                     const FloorLightContact& contact,
                                     SDL_FPoint& out_screen);
bool sample_floor_light_footprint_axes_px(const WarpedScreenGrid& cam,
                                          const FloorLightContact& contact,
                                          const SDL_FPoint& floor_screen_center,
                                          float base_radius_world,
                                          float height_spread_scale,
                                          float& out_radius_x_px,
                                          float& out_radius_y_px);
float floor_light_depth_weight(float abs_depth_from_anchor, float floor_light_cull_depth);
float floor_light_height_normalized(float world_height, float base_radius_world);
float floor_light_height_weight(float world_height, float base_radius_world);
float floor_light_height_spread_scale(float world_height, float base_radius_world);
float floor_light_footprint_radius(float base_radius_px, float world_height);
float layer_light_strength_multiplier_for_depth(double depth_from_camera_plane,
                                                float front_multiplier,
                                                float behind_multiplier);
float apply_layer_light_strength_bias(float intensity,
                                      double depth_from_camera_plane,
                                      float front_multiplier,
                                      float behind_multiplier);
bool light_overlaps_layer_slice(const LayerEffectProcessor::RuntimeLight& light,
                                double layer_depth_min,
                                double layer_depth_max,
                                float layer_bounds_min_x,
                                float layer_bounds_min_y,
                                float layer_bounds_max_x,
                                float layer_bounds_max_y);
bool dof_blur_chain_enabled(bool depth_of_field_enabled,
                            float blur_px,
                            float radial_blur_px);
std::vector<int> distributed_blur_repeat_counts(std::size_t target_blur_pass_count,
                                                std::size_t layer_count);
std::vector<int> background_chain_layers(const std::vector<int>& non_empty_layers, int player_layer_index);
std::vector<int> foreground_chain_layers(const std::vector<int>& non_empty_layers, int player_layer_index);
}

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

    struct RuntimeLightFadeState {
        float intensity_current = 0.0f;
        std::uint64_t last_seen_frame = 0;
    };

    struct RuntimeLightDebugOverlayEntry {
        SDL_FPoint center{0.0f, 0.0f};
        float radius = 0.0f;
        bool rendered = false;
    };

    struct PrevalidatedTag {};

    SceneRenderer(PrevalidatedTag, SDL_Renderer* renderer, Assets* assets, int screen_width, int screen_height, const nlohmann::json& map_manifest, const std::string& map_id);
    static PrevalidatedTag require_prerequisites(SDL_Renderer* renderer, Assets* assets);

    bool ensure_sky_texture();
    void destroy_sky_texture();
    bool ensure_mountain_texture();
    void destroy_mountain_texture();
    bool ensure_floor_background_textures();
    SDL_Texture* ensure_floor_light_falloff_texture();
    void render_floor_background_layer(const WarpedScreenGrid& cam,
                                       const world::WorldGrid& grid,
                                       const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
                                       bool runtime_lighting_enabled,
                                       double max_cull_depth,
                                       SDL_Texture* gameplay_target,
                                       bool render_floor_tiles);
    void render_sky_layer(const WarpedScreenGrid& cam,
                          double anchor_depth,
                          double max_cull_depth);
    void render_mountain_layer(const WarpedScreenGrid& cam,
                               double anchor_depth,
                               double max_cull_depth);
    void refresh_movement_debug_snapshots(const std::vector<Asset*>& visible_assets);
    void render_movement_debug_snapshots(const WarpedScreenGrid& cam,
                                         int screen_width,
                                         int screen_height,
                                         const std::vector<Asset*>& visible_assets) const;
    void gather_runtime_lights(const WarpedScreenGrid& cam,
                               const std::vector<Asset*>& rendered_assets,
                               std::vector<LayerEffectProcessor::RuntimeLight>& out_lights);
    void render_light_culling_debug_overlay() const;

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
    std::unordered_map<std::string, RuntimeLightFadeState> runtime_light_fade_states_;
    std::vector<RuntimeLightDebugOverlayEntry> runtime_light_debug_overlay_;
    std::uint32_t runtime_light_rendered_count_ = 0;
    std::uint32_t runtime_light_culled_count_ = 0;
    std::uint64_t runtime_light_profile_last_log_ticks_ = 0;

    CompositeAssetRenderer composite_renderer_;
    LayerEffectProcessor layer_effect_processor_;
    std::unique_ptr<DynamicBoundarySystem> dynamic_boundary_system_;

    SDL_Texture* scene_composite_tex_ = nullptr;
    SDL_Texture* postprocess_tex_     = nullptr;
    SDL_Texture* blur_tex_            = nullptr;
    SDL_Texture* background_seed_tex_ = nullptr;
    SDL_Texture* background_mid_tex_ = nullptr;
    SDL_Texture* foreground_mid_tex_ = nullptr;
    SDL_Texture* chain_temp_tex_ = nullptr;
    SDL_Texture* floor_base_texture_ = nullptr;
    SDL_Texture* floor_light_mask_texture_ = nullptr;
    SDL_Texture* floor_light_falloff_texture_ = nullptr;
    std::vector<SDL_Texture*> dof_layer_textures_;
    std::vector<SDL_Texture*> dof_dark_mask_textures_;
    std::vector<SDL_Texture*> dof_lit_textures_;
    std::vector<SDL_Texture*> dof_blur_textures_;
    std::filesystem::path sky_texture_path_;
    std::filesystem::path mountain_texture_path_;
    double                map_radius_world_ = 0.0;
    SDL_Texture*          sky_texture_       = nullptr;
    int                   sky_texture_width_ = 0;
    int                   sky_texture_height_ = 0;
    bool                  sky_texture_failed_ = false;
    SDL_Texture*          mountain_texture_ = nullptr;
    int                   mountain_texture_width_ = 0;
    int                   mountain_texture_height_ = 0;
    bool                  mountain_texture_failed_ = false;
};
