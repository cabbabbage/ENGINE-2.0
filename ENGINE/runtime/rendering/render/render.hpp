#pragma once

#include <array>
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

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "rendering/render/dynamic_boundary_system.hpp"
#include "rendering/render/debug_overlay_renderer.hpp"

class Assets;
class Asset;
class WarpedScreenGrid;
class AssetLibrary;
class FloorComposer;
class BlurChainRenderer;
class LayerStackRenderer;
class LayerSubmissionBuilder;
class SceneCompositePass;
namespace world { class WorldGrid; }

namespace render_internal {
struct FloorLightContact {
    float world_x = 0.0f;
    float world_z = 0.0f;
    float world_height = 0.0f;
    bool valid = false;
};

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
                                                float behind_multiplier,
                                                float transition_world = 0.0f);
float apply_layer_light_strength_bias(float intensity,
                                      double depth_from_camera_plane,
                                      float front_multiplier,
                                      float behind_multiplier,
                                      float transition_world = 0.0f);
bool light_overlaps_layer_slice(const LayerEffectProcessor::RuntimeLight& light,
                                double layer_depth_min,
                                double layer_depth_max,
                                float layer_bounds_min_x,
                                float layer_bounds_min_y,
                                float layer_bounds_max_x,
                                float layer_bounds_max_y,
                                float screen_padding_px = 0.0f,
                                float depth_padding_world = 0.0f);
bool dof_blur_chain_enabled(bool depth_of_field_enabled,
                            float blur_px,
                            float radial_blur_px);
std::vector<int> distributed_blur_repeat_counts(std::size_t target_blur_pass_count,
                                                std::size_t layer_count);
std::vector<int> background_chain_layers(const std::vector<int>& non_empty_layers, int player_layer_index);
std::vector<int> foreground_chain_layers(const std::vector<int>& non_empty_layers, int player_layer_index);
} // namespace render_internal

class GeometryBatcher {
public:
    explicit GeometryBatcher(SDL_Renderer* renderer);

    struct DrawItem {
        SDL_Texture* texture = nullptr;
        SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
        SDL_Vertex vertices[4];
        double depth = 0.0;
    };

    void addQuad(SDL_Texture* texture,
                 const SDL_Vertex vertices[4],
                 const int indices[6],
                 SDL_BlendMode blend_mode,
                 double depth);

    void flush();
    void clear();
    void for_each_item_far_to_near(const std::function<void(const DrawItem&)>& fn) const;

    size_t getDrawCallCount() const { return draw_call_count_; }
    size_t getTotalVertices() const { return total_vertices_; }
    double getLastFlushCpuMs() const { return last_flush_cpu_ms_; }

private:
    struct DepthBucket {
        std::vector<DrawItem> items;
    };

    SDL_Renderer* renderer_ = nullptr;
    std::map<std::int64_t, DepthBucket> depth_buckets_;
    DepthBucket invalid_depth_bucket_;
    size_t draw_call_count_ = 0;
    size_t total_vertices_ = 0;
    double last_flush_cpu_ms_ = 0.0;
    std::vector<SDL_Vertex> vertex_buffer_;
    std::vector<int> index_buffer_;
};

class SceneRenderer {
public:
    SceneRenderer(SDL_Renderer* renderer,
                  Assets* assets,
                  int screen_width,
                  int screen_height,
                  const nlohmann::json& map_manifest,
                  const std::string& map_id);
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
    void set_map_clear_color(SDL_Color color) { map_clear_color_ = color; }
    SDL_Color map_clear_color() const { return map_clear_color_; }

private:
    struct LightKey {
        Asset* asset = nullptr;
        std::uint32_t anchor_name_hash = 0;
    };
    struct LightKeyHash {
        std::size_t operator()(const LightKey& key) const noexcept {
            const std::size_t asset_hash = std::hash<Asset*>{}(key.asset);
            const std::size_t anchor_hash = static_cast<std::size_t>(key.anchor_name_hash);
            return asset_hash ^ (anchor_hash + 0x9e3779b97f4a7c15ULL + (asset_hash << 6U) + (asset_hash >> 2U));
        }
    };
    struct LightKeyEqual {
        bool operator()(const LightKey& lhs, const LightKey& rhs) const noexcept {
            return lhs.asset == rhs.asset && lhs.anchor_name_hash == rhs.anchor_name_hash;
        }
    };

    struct RuntimeLightFadeState {
        float intensity_current = 0.0f;
        std::uint64_t last_seen_frame = 0;
    };
    struct RuntimeLightCacheEntry {
        LayerEffectProcessor::RuntimeLight instance{};
        RuntimeLightFadeState fade{};
        std::uint64_t last_seen_frame = 0;
    };

    struct PrevalidatedTag {};

    static PrevalidatedTag require_prerequisites(SDL_Renderer* renderer, Assets* assets);

    SceneRenderer(PrevalidatedTag,
                  SDL_Renderer* renderer,
                  Assets* assets,
                  int screen_width,
                  int screen_height,
                  const nlohmann::json& map_manifest,
                  const std::string& map_id);

    bool ensure_scene_target();
    void collect_frame_geometry(const WarpedScreenGrid& cam,
                                world::WorldGrid& grid,
                                double focus_plane_world_z,
                                double max_cull_depth,
                                std::vector<Asset*>& rendered_assets_for_debug);
    void gather_runtime_lights(const WarpedScreenGrid& cam,
                            double focus_plane_world_z,
                            const std::vector<Asset*>& rendered_assets,
                            std::vector<LayerEffectProcessor::RuntimeLight>& out_lights);
    void refresh_movement_debug_snapshots(const std::vector<Asset*>& visible_assets);

    SDL_Renderer* renderer_ = nullptr;
    Assets* assets_ = nullptr;
    int screen_width_ = 1;
    int screen_height_ = 1;
    SDL_Color map_clear_color_{69, 101, 74, 255};

    SDL_Texture* scene_composite_tex_ = nullptr;
    double map_radius_world_ = 0.0;

    std::unique_ptr<GeometryBatcher> geometry_batcher_;
    std::unique_ptr<DynamicBoundarySystem> dynamic_boundary_system_;
    std::unique_ptr<FloorComposer> floor_composer_;
    std::unique_ptr<BlurChainRenderer> blur_chain_renderer_;
    std::unique_ptr<LayerStackRenderer> layer_stack_renderer_;
    std::unique_ptr<LayerSubmissionBuilder> layer_submission_builder_;
    std::unique_ptr<SceneCompositePass> scene_composite_pass_;
    std::unique_ptr<DebugOverlayRenderer> debug_overlay_renderer_;

    bool debug_auto_paths_ = false;
    bool movement_debug_visible_ = true;
    bool anchor_point_debug_enabled_ = false;

    std::unordered_map<const Asset*, render_debug::MovementDebugAssetSnapshot> movement_debug_snapshots_;
    std::unordered_map<const Asset*, render_debug::MovementDebugObservedState> movement_debug_observed_state_;
    std::vector<render_debug::RuntimeLightDebugOverlayEntry> runtime_light_debug_overlay_;

    std::unordered_map<LightKey, RuntimeLightCacheEntry, LightKeyHash, LightKeyEqual> runtime_light_cache_;
    std::uint64_t runtime_light_profile_last_log_ticks_ = 0;
    int runtime_light_rendered_count_ = 0;
    int runtime_light_culled_count_ = 0;
};
