#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "assets/asset/anchor_point.hpp"
#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "rendering/render/dynamic_boundary_system.hpp"
#include "rendering/render/debug_overlay_renderer.hpp"
#include "rendering/render/render_object.hpp"
#include "rendering/render/render_object_builder.hpp"

class Assets;
class Asset;
class WarpedScreenGrid;
class AssetLibrary;
class FloorComposer;
class BlurChainRenderer;
class LayerStackRenderer;
class LayerSubmissionBuilder;
class SceneCompositePass;
class GpuSceneRenderer;
namespace world { class WorldGrid; }

namespace render_internal {
struct DepthInterval {
    double min = 0.0;
    double max = 0.0;
};

struct ScreenAabb {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

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
DepthInterval make_sorted_depth_interval(double depth_min, double depth_max);
DepthInterval light_depth_interval(const LayerEffectProcessor::RuntimeLight& light);
int compare_depth_intervals_signed(const DepthInterval& light_interval, const DepthInterval& layer_interval);
bool screen_aabb_overlaps(const ScreenAabb& lhs, const ScreenAabb& rhs);
float layer_light_strength_multiplier_for_depth_interval(int signed_depth_separation,
                                                float front_multiplier,
                                                float behind_multiplier);
float apply_layer_light_strength_bias(float intensity,
                                      int signed_depth_separation,
                                      float front_multiplier,
                                      float behind_multiplier);
bool light_overlaps_layer_slice(const LayerEffectProcessor::RuntimeLight& light,
                                const DepthInterval& light_interval,
                                const DepthInterval& layer_interval,
                                const ScreenAabb& light_bounds,
                                const ScreenAabb& layer_bounds);
bool dof_blur_chain_enabled(bool depth_of_field_enabled,
                            float blur_px,
                            float radial_blur_px);
std::vector<int> distributed_blur_repeat_counts(std::size_t target_blur_pass_count,
                                                std::size_t layer_count);
float dof_quality_scale(int screen_width,
                        int screen_height,
                        float blur_px,
                        float radial_blur_px);
std::vector<int> background_chain_layers(const std::vector<int>& non_empty_layers, int player_layer_index);
std::vector<int> foreground_chain_layers(const std::vector<int>& non_empty_layers, int player_layer_index);
} // namespace render_internal

class GeometryBatcher {
public:
    explicit GeometryBatcher(SDL_Renderer* renderer);
    static constexpr int kMaxVerticesPerItem = 6;
    static constexpr int kMaxIndicesPerItem = 12;

    struct DrawItem {
        SDL_Texture* texture = nullptr;
        SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
        SDL_Vertex vertices[kMaxVerticesPerItem]{};
        int indices[kMaxIndicesPerItem]{};
        int vertex_count = 0;
        int index_count = 0;
        double depth = 0.0;
    };

    void addQuad(SDL_Texture* texture,
                 const SDL_Vertex vertices[4],
                 const int indices[6],
                 SDL_BlendMode blend_mode,
                 double depth);
    void addGeometry(SDL_Texture* texture,
                     const SDL_Vertex* vertices,
                     int vertex_count,
                     const int* indices,
                     int index_count,
                     SDL_BlendMode blend_mode,
                     double depth);

    void flush();
    void clear();
    void for_each_item_far_to_near(const std::function<void(const DrawItem&)>& fn) const;
    std::size_t item_count() const;

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
    enum class RuntimeRendererMode {
        Gpu
    };

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
    bool gpu_runtime_path_enabled() const { return gpu_runtime_path_enabled_; }
    RuntimeRendererMode runtime_renderer_mode() const { return runtime_renderer_mode_; }
    const std::string& runtime_renderer_mode_name() const { return runtime_renderer_mode_name_; }

private:
    struct RuntimeLightRegistryKey {
        const Asset* asset = nullptr;
        std::string anchor_name;

        bool operator==(const RuntimeLightRegistryKey& other) const {
            return asset == other.asset && anchor_name == other.anchor_name;
        }
    };
    struct RuntimeLightRegistryKeyHash {
        std::size_t operator()(const RuntimeLightRegistryKey& key) const {
            const std::size_t asset_hash = std::hash<const Asset*>{}(key.asset);
            const std::size_t anchor_hash = std::hash<std::string>{}(key.anchor_name);
            return asset_hash ^ (anchor_hash + 0x9e3779b9u + (asset_hash << 6) + (asset_hash >> 2));
        }
    };
    struct RuntimeLightSpatialCell {
        int x = 0;
        int z = 0;

        bool operator==(const RuntimeLightSpatialCell& other) const {
            return x == other.x && z == other.z;
        }
    };
    struct RuntimeLightSpatialCellHash {
        std::size_t operator()(const RuntimeLightSpatialCell& cell) const {
            const std::size_t x_hash = std::hash<int>{}(cell.x);
            const std::size_t z_hash = std::hash<int>{}(cell.z);
            return x_hash ^ (z_hash + 0x9e3779b9u + (x_hash << 6) + (x_hash >> 2));
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
    struct RuntimeLightRegistryEntry {
        std::uint32_t light_id = 0;
        Asset* asset = nullptr;
        std::string anchor_name;
        AnchorLightData light{};
        float anchor_world_x = 0.0f;
        float anchor_world_y = 0.0f;
        float anchor_world_z = 0.0f;
        float radius_world = AnchorLightData::kMinRadius;
        bool hidden = false;
        bool valid = false;
        RuntimeLightSpatialCell cell{};
        std::uint64_t last_seen_frame = 0;
        bool transform_dirty = true;
        bool frame_dirty = true;
        bool light_data_dirty = true;
        bool removed = false;
    };
    struct RuntimeLightAssetState {
        std::uint64_t anchor_revision = 0;
        int frame_index = std::numeric_limits<int>::min();
        std::size_t anchor_light_signature = 0;
        bool alive = true;
    };
    struct RuntimeLightBroadphaseEntry {
        std::uint32_t light_id = 0;
        SDL_FPoint screen{0.0f, 0.0f};
        float radius_px = 0.0f;
    };
    struct AssetRenderCacheEntry {
        render_build::DirectAssetRenderCacheRecord static_record{};
        RenderObject object{};
        bool initialized = false;
        int frame_identity = -1;
        int variant_identity = -1;
        SDL_Texture* texture_identity = nullptr;
        Uint32 reprojection_identity = 0;
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
    bool ensure_far_backdrop_resources();
    bool ensure_far_backdrop_composite_target();
    std::filesystem::path resolve_misc_content_path(const std::string& filename) const;
    void destroy_far_backdrop_resources();
    void collect_frame_geometry(const WarpedScreenGrid& cam,
                                world::WorldGrid& grid,
                                double focus_plane_world_z,
                                double max_cull_depth,
                                std::vector<Asset*>& rendered_assets_for_debug);
    void gather_runtime_lights(const WarpedScreenGrid& cam,
                            double focus_plane_world_z,
                            const std::vector<Asset*>& rendered_assets,
                            std::vector<LayerEffectProcessor::RuntimeLight>& out_lights);
    void update_runtime_light_registry_incremental(std::uint64_t frame_token);
    void enqueue_runtime_light_dirty(std::uint32_t light_id, RuntimeLightRegistryEntry& entry,
                                     bool transform_dirty, bool frame_dirty, bool light_data_dirty, bool removed);
    void prune_removed_runtime_lights(std::uint64_t frame_token);
    void discover_runtime_lights_for_asset(Asset* asset, std::uint64_t frame_token);
    RuntimeLightSpatialCell runtime_light_cell_for_world(float world_x, float world_z) const;
    void runtime_light_query_visible_cells(const WarpedScreenGrid& cam,
                                           float world_z,
                                           float culling_margin,
                                           std::vector<std::uint32_t>& out_light_ids) const;
    void refresh_movement_debug_snapshots(const std::vector<Asset*>& visible_assets);

    SDL_Renderer* renderer_ = nullptr;
    Assets* assets_ = nullptr;
    int screen_width_ = 1;
    int screen_height_ = 1;
    SDL_Color map_clear_color_{69, 101, 74, 255};

    SDL_Texture* scene_composite_tex_ = nullptr;
    SDL_Texture* scene_preblur_tex_ = nullptr;
    SDL_Texture* far_backdrop_sky_tex_ = nullptr;
    SDL_Texture* far_backdrop_mountains_tex_ = nullptr;
    double map_radius_world_ = 0.0;

    std::unique_ptr<GeometryBatcher> geometry_batcher_;
    std::unique_ptr<DynamicBoundarySystem> dynamic_boundary_system_;
    std::unique_ptr<FloorComposer> floor_composer_;
    std::unique_ptr<BlurChainRenderer> blur_chain_renderer_;
    std::unique_ptr<LayerStackRenderer> layer_stack_renderer_;
    std::unique_ptr<LayerSubmissionBuilder> layer_submission_builder_;
    std::unique_ptr<SceneCompositePass> scene_composite_pass_;
    std::unique_ptr<DebugOverlayRenderer> debug_overlay_renderer_;
    std::unique_ptr<GpuSceneRenderer> gpu_scene_renderer_;
    bool gpu_runtime_path_enabled_ = false;
    RuntimeRendererMode runtime_renderer_mode_ = RuntimeRendererMode::Gpu;
    std::string runtime_renderer_mode_name_ = "gpu";

    bool debug_auto_paths_ = false;
    bool movement_debug_visible_ = true;
    bool anchor_point_debug_enabled_ = false;
    bool first_gpu_submission_pending_ = true;

    std::unordered_map<const Asset*, render_debug::MovementDebugAssetSnapshot> movement_debug_snapshots_;
    std::unordered_map<const Asset*, render_debug::MovementDebugObservedState> movement_debug_observed_state_;
    std::unordered_map<const Asset*, AssetRenderCacheEntry> asset_render_cache_;
    std::vector<render_debug::RuntimeLightDebugOverlayEntry> runtime_light_debug_overlay_;

    std::unordered_map<RuntimeLightRegistryKey, std::uint32_t, RuntimeLightRegistryKeyHash> runtime_light_registry_ids_;
    std::vector<RuntimeLightRegistryEntry> runtime_light_registry_entries_;
    std::unordered_map<RuntimeLightSpatialCell,
                       std::vector<std::uint32_t>,
                       RuntimeLightSpatialCellHash> runtime_light_spatial_index_;
    std::unordered_map<Asset*, RuntimeLightAssetState> runtime_light_asset_state_;
    std::vector<std::uint32_t> runtime_light_dirty_queue_;
    std::unordered_set<std::uint32_t> runtime_light_dirty_set_;
    std::vector<RuntimeLightCacheEntry> runtime_light_cache_;
    std::uint32_t runtime_light_next_id_ = 1;
    int runtime_light_spatial_cell_size_ = 256;
    float runtime_light_max_radius_world_ = AnchorLightData::kMinRadius;
    std::uint64_t runtime_light_profile_last_log_ticks_ = 0;
    int runtime_light_rendered_count_ = 0;
    int runtime_light_culled_count_ = 0;
    std::uint64_t runtime_light_observed_active_state_version_ = std::numeric_limits<std::uint64_t>::max();
    int runtime_light_debug_parity_visible_count_ = 0;
};
