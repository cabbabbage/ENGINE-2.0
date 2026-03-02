//TODO we need to implement a public height() that should return the current height in screen pixles of our asset
//we need to add a public method to return the current grid point of the asset
#ifndef ASSET_HPP
#define ASSET_HPP

#include <string>
#include <array>
#include <vector>
#include <memory>
#include <optional>
#include <SDL3/SDL.h>
#include <limits>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <cmath>

#include "utils/area.hpp"
#include "asset_info.hpp"
#include "gameplay/world/grid_point.hpp"
#include "anchor_point.hpp"
#include "utils/AnchorPointResolver.hpp"

#include "utils/transform_smoothing.hpp"

#include "asset_controller.hpp"
#include "animation/animation_update.hpp"
#include "animation/attack.hpp"
#include "rendering/render/render.hpp"

class WarpedScreenGrid;
class Assets;
class Input;
class AnimationFrame;
class Animation;
class AssetInfoUI;
class RenderAsset;
class AssetList;

namespace world { class WorldGrid; }

using world::GridPoint;

struct RenderObject {
    SDL_Texture* texture = nullptr;
    SDL_Rect screen_rect{};
    SDL_Color color_mod{255, 255, 255, 255};
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    double angle = 0.0;
    SDL_Point center{0, 0};
    bool use_custom_center = false;
    SDL_FlipMode flip = SDL_FLIP_NONE;
    int texture_w = 0;
    int texture_h = 0;
    bool has_texture_size = false;
    float world_z_offset = 0.0f;
    bool has_src_rect = false;
    SDL_Rect src_rect{0, 0, 0, 0};
    std::vector<SDL_Vertex> cached_vertices;
    std::vector<int> cached_indices;
    SDL_FPoint cached_position{0.0f, 0.0f};
    float cached_scale = 0.0f;
    std::uint64_t cached_camera_state_version = 0;
    bool mesh_dirty = true;
};

using RenderCompositePackage = std::vector<RenderObject>;

struct DepthCueRenderData {
    SDL_Texture* base_texture = nullptr;
    SDL_Texture* foreground_texture = nullptr;
    SDL_Texture* background_texture = nullptr;
    bool has_depth_cue = false;
};

class Asset {

        public:
    RenderCompositePackage render_package;

    struct TilingInfo {
        bool      enabled      = false;
        SDL_Point grid_origin{0, 0};
        SDL_Point tile_size{0, 0};
        SDL_Rect  coverage{0, 0, 0, 0};
        SDL_Point anchor{0, 0};

        bool is_valid() const {
            return enabled && tile_size.x > 0 && tile_size.y > 0 && coverage.w > 0 && coverage.h > 0;
        }
};


    Area get_area(const std::string& name) const;
    Asset(std::shared_ptr<AssetInfo> info,
          const Area& spawn_area,
          SDL_Point start_pos,
          int depth,
          const std::string& spawn_id = std::string{},
          const std::string& spawn_method = std::string{},
          int grid_resolution = 0);
    Asset(const Asset& other);
    Asset& operator=(const Asset& other);
    Asset(Asset&&) noexcept = default;
    Asset& operator=(Asset&&) noexcept = default;
    ~Asset();
    void finalize_setup();
    void rebuild_animation_runtime();
    void Delete();

    bool is_finalized() const { return finalized_; }
    void on_scale_factor_changed();

    // 3D Grid Position accessors
    GridPoint* grid_point() const { return pos_; }
    int world_x() const { return pos_ ? pos_->world_x() : initial_world_pos_.x; }
    int world_y() const { return pos_ ? pos_->world_y() : initial_world_pos_.y; }
    int world_z() const { return pos_ ? pos_->world_z() : 0; }
    SDL_Point world_point() const { return SDL_Point{world_x(), world_y()}; }
    int height() const {
        if (cached_h <= 0) {
            // Refresh lazily to avoid stale dimensions when textures change.
            const_cast<Asset*>(this)->refresh_cached_dimensions();
        }
        return cached_h;
    }
    float runtime_height_px() const;
    void move_to_world_position(int world_x, int world_y, int world_z = 0,
                                std::optional<int> resolution_layer_override = std::nullopt);
    void set_world_z(int world_z);

    void update();
    SDL_Texture* get_current_frame() const;
    std::string get_current_animation() const;
    bool is_current_animation_locked_in_progress() const;
    bool is_current_animation_last_frame() const;
    bool is_current_animation_looping() const;
    const AnimationFrame* current_animation_frame() const { return current_frame; }

    struct ScaleUsageStats {
        float requested_scale = 1.0f;
        float texture_scale   = 1.0f;
        float remainder_scale = 1.0f;
        int   variant_index   = 0;

        float requested_percent() const { return requested_scale * 100.0f; }
        float texture_percent() const { return texture_scale * 100.0f; }
        float remainder_percent() const { return remainder_scale * 100.0f; }
};

    const ScaleUsageStats& last_scale_usage() const { return last_scale_usage_; }
    struct ScaleVariantState {
        int   last_variant_index = 0;
        float hysteresis_min     = 0.0f;
        float hysteresis_max     = std::numeric_limits<float>::max();
};

    const ScaleVariantState& scale_variant_state() const { return scale_variant_state_; }

    void set_frame_progress(float p) { frame_progress = p; }
    class AnimationFrame* current_frame = nullptr;
    const AnimationFrame* last_rendered_frame() const { return last_rendered_frame_; }
    void set_last_rendered_frame(const AnimationFrame* frame) { last_rendered_frame_ = frame; }
    void reset_last_rendered_frame() { last_rendered_frame_ = nullptr; }
    SDL_Texture* get_texture();
    void set_camera(WarpedScreenGrid* v) { window = v; }
    void set_assets(Assets* a);
    Assets* get_assets() const { return assets_; }
    void set_tiling_info(std::optional<TilingInfo> info);
    const std::optional<TilingInfo>& tiling_info() const { return tiling_info_; }
    const std::string& owning_room_name() const { return owning_room_name_; }
    void set_owning_room_name(std::string name);
    AssetList* get_neighbors_list();
    const AssetList* get_neighbors_list() const;
    AssetList* get_impassable_naighbors();
    const AssetList* get_impassable_naighbors() const;
    void deactivate();
    int NeighborSearchRadius;
    void set_hidden(bool state);
    bool is_hidden() const;
    void set_anchor_hidden(bool state);
    bool is_anchor_hidden() const;
    void set_highlighted(bool state);
    bool is_highlighted();
    void set_selected(bool state);
    bool is_selected();
    void set_merged_from_neighbors(bool state);
    bool merged_from_neighbors() const;
    void cache_grid_residency(const world::GridPoint& point);
    void clear_grid_residency_cache();
    bool has_grid_residency_cache() const;
    world::GridKey grid_residency_cache() const;
    void sync_transform_to_position();
    void set_grid_id(std::uint64_t id);
    std::uint64_t grid_id() const { return grid_id_; }
    void clear_grid_id();
    void set_world_z_offset(float z) { if (world_z_offset_ != z) { world_z_offset_ = z; mark_anchors_dirty(); } }
    float world_z_offset() const { return world_z_offset_; }

    SDL_Texture* composite_texture() const { return composite_texture_; }
    void set_composite_texture(SDL_Texture* tex);
    bool is_composite_dirty() const { return composite_dirty_; }
    void mark_composite_dirty() { composite_dirty_ = true; }
    void clear_composite_dirty() { composite_dirty_ = false; }
    const SDL_Rect& composite_rect() const { return composite_rect_; }
    void set_composite_rect(const SDL_Rect& r) { composite_rect_ = r; }
    float        composite_scale() const { return composite_scale_; }
    bool is_mesh_dirty() const { return mesh_dirty_; }
    void mark_mesh_dirty() { mesh_dirty_ = true; }
    void clear_mesh_dirty() { mesh_dirty_ = false; }
    void refresh_frame_texture_bindings();


    float smoothed_translation_x() const;
    float smoothed_translation_y() const;
    float smoothed_scale() const;
    float smoothed_alpha() const;
    std::shared_ptr<AssetInfo> info;
    std::string current_animation;
    int grid_resolution = 0;
    bool active = false;
    bool flipped = false;
    float distance_from_camera = 0.0f;
    float angle_from_camera = 0.0f;


    int depth = 0;
    bool dead = false;
    bool static_frame = true;
    bool needs_target = false;
    bool target_reached = false;
    int cached_w = 0;
    int cached_h = 0;
    std::uint64_t last_render_frame_id = 0;
    std::uint64_t visibility_stamp = 0;
    std::uint32_t last_visible_frame_id = 0;
    std::uint32_t last_active_frame_id = 0;
    std::string spawn_id;
    std::string spawn_method;
    const std::string& filter_type_tag() const { return filter_type_tag_; }
    const std::string& filter_method_tag() const { return filter_method_tag_; }
    std::string owning_room_name_;
    std::unique_ptr<AnimationUpdate> anim_;
    std::unique_ptr<class AnimationRuntime> anim_runtime_;
    float current_scale = 1.00f;
    float current_nearest_variant_scale = 1.00f;
    float current_remaining_scale_adjustment = 1.00f;
    int   current_variant_index = 0;

    // Cache the last scale inputs so we can skip redundant work when nothing changed.
    float last_scale_base_input_ = -1.0f;
    float last_scale_perspective_input_ = -1.0f;
    float last_scale_camera_input_ = -1.0f;

    void update_scale_values();
    SDL_Texture* get_current_variant_texture() const;
    void set_current_animation(const std::string& name);
    // Queue an attack event for deferred controller handling.
    void send_attack(const animation_update::Attack& attack);
    // Drain and return queued attacks for this tick.
    std::vector<animation_update::Attack> process_pending_attacks();

    struct AnchorHandle {
        std::string     name;
        world::GridPoint* grid = nullptr;
        SDL_Point       world_px{0, 0};
        int             world_z = 0;
        int             resolution_layer = 0;
        SDL_Point       source_texture_px{0, 0};
        bool            has_canonical_texture_source = false;
        bool            dirty = true;
        bool            missing = false;
        bool            in_front = true;
        Asset*          owner = nullptr;
        struct UpdateKey {
                anchor_points::GridMaterialization grid_policy = anchor_points::GridMaterialization::None;
                std::optional<anchor_points::AnchorDepthPolicy> depth_policy{};
                bool initialized = false;

                bool matches(anchor_points::GridMaterialization grid,
                             const std::optional<anchor_points::AnchorDepthPolicy>& depth) const {
                        return initialized && grid_policy == grid && depth_policy == depth;
                }

                void set(anchor_points::GridMaterialization grid,
                         std::optional<anchor_points::AnchorDepthPolicy> depth) {
                        grid_policy = grid;
                        depth_policy = depth;
                        initialized = true;
                }
        } last_update_key_;

        void update(anchor_points::GridMaterialization grid_policy,
                    std::optional<anchor_points::AnchorDepthPolicy> depth_policy = std::nullopt);
    };

    // A single source of truth for all inputs that influence resolved anchor world position.
    struct AnchorBasisSignature {
        int   world_x = 0;
        int   world_y = 0;
        int   world_z = 0;
        int   frame_index = 0;
        int   variant_index = 0;
        bool  flipped = false;
        float remainder_scale = 1.0f;       // runtime scale applied to rendered frame geometry
        float perspective_scale = 1.0f;     // depth-based scaling from the grid/camera
        float world_z_offset = 0.0f;        // vertical offset fed into anchor projection
        int   resolution_layer = 0;         // grid resolution used for anchor materialization
    };

    AnchorPoint* get_anchor_point(const std::string& name);
    std::optional<AnchorPoint> anchor_state(const std::string& name,
                                            anchor_points::GridMaterialization grid_policy = anchor_points::GridMaterialization::None,
                                            std::optional<anchor_points::AnchorDepthPolicy> depth_policy = std::nullopt);
    AnchorPoint& resolve_anchor_point_entry(std::size_t index,
                                            anchor_points::GridMaterialization grid_policy,
                                            std::optional<anchor_points::AnchorDepthPolicy> depth_policy,
                                            const DisplacedAssetAnchorPoint* frame_anchor);
    void refresh_anchor_point_cache_from_frame();
    void mark_anchors_dirty();
    bool update_anchor_basis_if_needed();
    AnchorBasisSignature compute_anchor_basis_signature() const;
    void capture_anchor_basis_snapshot(const AnchorBasisSignature& signature);

public:
    static void SetFlipOverrideForSpawnId(const std::string& spawn_id, bool enabled, bool flipped);
    static void ClearFlipOverrideForSpawnId(const std::string& spawn_id);
private:
    static std::unordered_map<std::string, std::pair<bool,bool>> s_flip_overrides_;
    static std::mutex s_flip_overrides_mutex_;
    friend class AnimationUpdate;
    friend class AnimationRuntime;
    friend class Move;
    friend class AssetInfoUI;
    friend class RenderAsset;
    friend class FrameEditorSession;
    friend class Assets;
    friend class CompositeAssetRenderer;
    friend class world::WorldGrid;
    WarpedScreenGrid* window = nullptr;
    bool highlighted = false;
    bool hidden = false;
    bool anchor_hidden_ = false;
    bool selected = false;
    bool merged_from_neighbors_ = false;
    GridPoint* pos_ = nullptr; // Non-owning pointer to GridPoint in WorldGrid; set by WorldGrid on registration
    SDL_Point initial_world_pos_{0, 0}; // Spawn position, used before registration with WorldGrid
    void set_flip();

    float frame_progress = 0.0f;
    Assets* assets_ = nullptr;
    std::unique_ptr<AssetController>   controller_;
    std::unique_ptr<AssetList> neighbors;
    std::unique_ptr<AssetList> impassable_naighbors;



    std::optional<TilingInfo> tiling_info_{};
    SDL_Point last_neighbor_origin_{ std::numeric_limits<int>::min(), std::numeric_limits<int>::min() };
    bool neighbor_lists_initialized_ = false;
    void update_neighbor_lists(bool force_update);
    void ensure_animation_runtime(bool force_recreate);

    void clear_downscale_cache();
    void invalidate_downscale_cache();
    void refresh_cached_dimensions();
    void recompute_local_bounds_square();

    std::uint64_t downscale_cache_ready_revision_ = 0;

    SDL_Rect     composite_bounds_local_{0, 0, 0, 0};

    SDL_Texture* last_scaled_texture_      = nullptr;
    SDL_Texture* last_scaled_source_       = nullptr;
    int          last_scaled_w_            = 0;
    int          last_scaled_h_            = 0;
    float        last_scaled_camera_scale_ = -1.0f;

    ScaleUsageStats last_scale_usage_{};
    ScaleVariantState scale_variant_state_{};

    void clear_render_caches();

    TransformSmoothingState alpha_smoothing_{};
    // Protect the pending attack queue for thread-safe enqueue/dequeue.
    std::mutex pending_attacks_mutex_;
    std::vector<animation_update::Attack> pending_attacks_;

    const AnimationFrame* last_rendered_frame_ = nullptr;

    bool finalized_ = false;
    std::uint64_t grid_id_ = 0;
    bool has_cached_grid_residency_ = false;
    world::GridKey cached_grid_residency_{0, 0, 0, 0};
    SDL_Texture* composite_texture_ = nullptr;
    bool         composite_dirty_   = true;
    SDL_Rect     composite_rect_    = {0, 0, 0, 0};
    float        composite_scale_   = 1.0f;
    float        world_z_offset_    = 0.0f;
    bool         mesh_dirty_        = true;

    void initialize_anchor_registry_from_animations();
    AnchorHandle* find_anchor_handle(const std::string& name);

    std::vector<AnchorHandle> anchor_handles_;
    std::vector<AnchorPoint> anchor_points_;
    std::unordered_map<std::string, std::size_t> anchor_name_to_index_;
    bool anchors_initialized_ = false;

    std::uint64_t anchor_world_revision_ = 1;
    AnchorBasisSignature last_anchor_basis_signature_{};
    bool anchor_basis_initialized_ = false;

    void refresh_filter_tags();

    std::string filter_type_tag_;
    std::string filter_method_tag_;

};

#endif
