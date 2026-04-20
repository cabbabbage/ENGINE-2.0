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
#include "gameplay/spawn/runtime_candidates.hpp"
#include "gameplay/world/grid_point.hpp"
#include "anchor_point.hpp"
#include "utils/AnchorPointResolver.hpp"

#include "utils/transform_smoothing.hpp"

#include "asset_controller.hpp"
#include "animation/animation_update.hpp"
#include "animation/attack.hpp"
#include "rendering/render/scaling_logic.hpp"

class WarpedScreenGrid;
class Assets;
class Input;
class AnimationFrame;
class Animation;
class AssetInfoUI;
class RenderAsset;

namespace world { class WorldGrid; }

using world::GridPoint;

struct RuntimeCameraMetrics {
    std::uint64_t frame_id = 0;
    std::uint64_t camera_state_version = 0;
    std::uint64_t anchor_revision = 0;
    bool valid = false;
    float planar_dx = 0.0f;
    float planar_dz = 0.0f;
    float planar_distance = 0.0f;
    float planar_angle_radians = 0.0f;
    float anchor_world_z = 0.0f;
    float depth_axis_sign = 1.0f;
    float world_z_depth_offset = 0.0f;
    float effective_world_z_depth_offset = 0.0f;
    double world_z_depth_from_anchor = 0.0;
    double effective_world_z_depth_from_anchor = 0.0;
};

class Asset {

        public:
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
    GridPoint* grid_point() const { return grid_point_; }
    int world_x() const {
        SDL_assert(grid_point_ != nullptr);
        return grid_point_ ? grid_point_->world_x() : 0;
    }
    int world_y() const {
        SDL_assert(grid_point_ != nullptr);
        return grid_point_ ? grid_point_->world_y() : 0;
    }
    int world_z() const {
        SDL_assert(grid_point_ != nullptr);
        return grid_point_ ? grid_point_->world_z() : 0;
    }
    std::uint64_t anchor_world_revision() const { return anchor_world_revision_; }
    bool has_fresh_runtime_camera_metrics(std::uint64_t frame_id,
                                          std::uint64_t camera_state_version) const {
        return runtime_camera_metrics.valid &&
               runtime_camera_metrics.frame_id == frame_id &&
               runtime_camera_metrics.camera_state_version == camera_state_version &&
               runtime_camera_metrics.anchor_revision == anchor_world_revision_;
    }
    SDL_Point world_xz_point() const { return SDL_Point{world_x(), world_z()}; }
    SDL_Point world_xy_point() const { return SDL_Point{world_x(), world_y()}; }
    int height() const {
        if (cached_h <= 0) {
            // Refresh lazily to avoid stale dimensions when textures change.
            const_cast<Asset*>(this)->refresh_cached_dimensions();
        }
        return cached_h;
    }
    int width() const {
        if (cached_w <= 0) {
            // Refresh lazily to avoid stale dimensions when textures change.
            const_cast<Asset*>(this)->refresh_cached_dimensions();
        }
        return cached_w;
    }
    float runtime_scale_remainder() const;
    float runtime_resolved_scale() const;
    float runtime_width_px() const;
    float runtime_height_px() const;
    float runtime_effective_base_scale() const;
    float size_variation_sample() const { return size_variation_sample_; }
    enum class PerspectiveSource {
        AnchorBindingOverride,
        CameraTraversal,
        AssetGridPoint,
        CachedLastFrame,
        Default
    };
    struct PerspectiveSample {
        float scale = 1.0f;
        int resolution_layer = 0;
        PerspectiveSource source = PerspectiveSource::Default;
    };
    PerspectiveSample runtime_perspective_sample() const;
    static const char* perspective_source_label(PerspectiveSource source);
    bool set_anchor_perspective_override(float scale,
                                         std::optional<int> resolution_layer_override = std::nullopt);
    bool clear_anchor_perspective_override();
    bool has_anchor_perspective_override() const { return anchor_perspective_override_active_; }
    void move_to_world_position(int world_x, int world_y, int world_z = 0,
                                std::optional<int> resolution_layer_override = std::nullopt);
    void set_world_z(int world_z);
    double render_depth_bias() const { return render_depth_bias_; }
    void set_render_depth_bias(double bias);

    void update();
    SDL_Texture* get_current_frame() const;
    std::string get_current_animation() const;
    bool is_current_animation_locked_in_progress() const;
    bool is_current_animation_last_frame() const;
    bool is_current_animation_looping() const;
    const AnimationFrame* current_animation_frame() const { return current_frame; }
    struct CumulativeMovementDisplacement {
        float dx = 0.0f;
        float dy = 0.0f;
        float dz = 0.0f;
        bool valid = false;
    };
    CumulativeMovementDisplacement current_frame_cumulative_movement_displacement() const;

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
    float frame_delta_seconds_clamped() const;
    void add_child(Asset* child);
    void remove_child(Asset* child);
    bool has_child(const Asset* child) const;
    std::vector<Asset*>& children() { return children_; }
    const std::vector<Asset*>& children() const { return children_; }
    void set_tiling_info(std::optional<TilingInfo> info);
    const std::optional<TilingInfo>& tiling_info() const { return tiling_info_; }
    const std::string& owning_room_name() const { return owning_room_name_; }
    void set_owning_room_name(std::string name);
    void deactivate();
    int NeighborSearchRadius;
    void set_hidden(bool state);
    bool is_hidden() const;
    void set_default_controller_animation_enforced(bool enforced) { enforce_default_controller_animation_ = enforced; }
    bool default_controller_animation_enforced() const { return enforce_default_controller_animation_; }
    void set_anchor_hidden(bool state);
    bool is_anchor_hidden() const;
    void set_highlighted(bool state);
    bool is_highlighted();
    void set_selected(bool state);
    bool is_selected();
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
    bool set_directional_heading_radians(float radians);
    void clear_directional_heading_radians();
    bool has_directional_heading_radians() const { return directional_heading_valid_; }
    float directional_heading_radians() const { return directional_heading_radians_; }
    bool set_directional_target_world_xz(float world_x, float world_z);
    void clear_directional_target_world_xz();
    bool has_directional_target_world_xz() const { return directional_target_valid_; }
    float directional_target_world_x() const { return directional_target_world_x_; }
    float directional_target_world_z() const { return directional_target_world_z_; }
    void set_render_anchor_offset(float x, float y, float z);
    void clear_render_anchor_offset();
    float render_anchor_offset_x() const { return render_anchor_offset_x_; }
    float render_anchor_offset_y() const { return render_anchor_offset_y_; }
    float render_anchor_offset_z() const { return render_anchor_offset_z_; }
    bool set_anchor_sprite_transform_override(SDL_FlipMode flip, double angle_degrees);
    bool clear_anchor_sprite_transform_override();
    bool has_anchor_sprite_transform_override() const { return anchor_sprite_transform_override_active_; }
    SDL_FlipMode effective_render_flip() const;
    double effective_render_angle() const;

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
    RuntimeCameraMetrics runtime_camera_metrics{};
    double render_depth_bias_ = 0.0;


    int depth = 0;
    bool dead = false;
    int runtime_health = 0;
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
    float last_scale_quality_cap_input_ = -1.0f;
    std::uint32_t last_scale_update_frame_id_ = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t last_scale_update_camera_state_version_ = std::numeric_limits<std::uint64_t>::max();

    void update_scale_values(bool force = false);
    SDL_Texture* get_current_variant_texture() const;
    void set_current_animation(const std::string& name);
    // Queue an attack event for deferred controller handling.
    void send_attack(const animation_update::Attack& attack);
    // Drain and return queued attacks for this tick.
    std::vector<animation_update::Attack> process_pending_attacks();

    struct RuntimeBoxPoint3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct RuntimeBoxVolume {
        std::string id;
        std::string type;
        std::string name;
        bool enabled = true;
        int frame_start = 0;
        int frame_end = 0;
        std::string anchor_link;
        int frame_index = -1;
        int extrusion_amount = 0;
        bool flatten_bottom_to_floor = false;
        int damage_amount = 0;
        std::string payload_id;
        std::string meta_json;
        animation_update::AttackPayload payload{};
        std::array<RuntimeBoxPoint3, 8> world_points{};
        RuntimeBoxPoint3 centroid{};
        bool valid = false;
    };

    struct RuntimeImpassableShape {
        std::string id;
        std::string name;
        bool enabled = true;
        std::vector<SDL_FPoint> floor_points{};
        std::vector<RuntimeBoxPoint3> bottom_ring{};
        std::vector<RuntimeBoxPoint3> top_ring{};
        RuntimeBoxPoint3 centroid{};
        bool valid = false;
    };

    struct RuntimeFloorBox {
        struct CandidatePayload {
            vibble::spawn::RuntimeCandidates candidates;
            int grid_resolution = 4;
            bool has_positive_non_null_candidate = false;
        };

        std::string id;
        std::string name;
        float position_x = 0.0f;
        float position_z = 0.0f;
        float width = 0.0f;
        float depth = 0.0f;
        bool enabled = true;
        std::vector<std::string> tags;
        std::optional<CandidatePayload> candidate;

        bool has_tag(const std::string& tag) const;
    };

    bool isMovementEnabled() const;
    bool isHitboxEnabled() const;
    bool isAttackBoxEnabled() const;
    bool isImpassableBoxEnabled() const;
    bool isFloorBoxesEnabled() const;
    bool affects_collision_context() const;
    const std::vector<RuntimeFloorBox>& getFloorBoxes() const;

    const std::vector<RuntimeBoxVolume>& current_hit_box_volumes() const { return current_hit_box_volumes_; }
    const std::vector<RuntimeBoxVolume>& current_attack_box_volumes() const { return current_attack_box_volumes_; }
    const std::vector<RuntimeBoxVolume>& current_impassable_box_volumes() const { return current_impassable_box_volumes_; }
    const std::vector<RuntimeImpassableShape>& current_impassable_shapes() const { return current_impassable_shapes_; }
    void test_set_current_hit_box_volumes(std::vector<RuntimeBoxVolume> volumes);
    void test_set_current_attack_box_volumes(std::vector<RuntimeBoxVolume> volumes);
    const RuntimeBoxVolume* find_hit_box_volume(const std::string& name) const;
    const RuntimeBoxVolume* find_attack_box_volume(const std::string& name) const;

    struct AnchorHandle {
        std::string     name;
        world::GridPoint* grid = nullptr;
        Vec2            world_exact_pos_2d{};
        float           world_exact_z = 0.0f;
        Vec2            flat_world_exact_pos_2d{};
        float           flat_world_exact_z = 0.0f;
        float           flat_perspective_scale = 1.0f;
        bool            has_flat_perspective_scale = false;
        SDL_Point       world_px{0, 0};
        int             world_z = 0;
        float           world_depth = 0.0f;
        int             resolution_layer = 0;
        SDL_Point       source_texture_px{0, 0};
        SDL_FPoint      screen_px{0.0f, 0.0f};
        bool            has_canonical_texture_source = false;
        bool            dirty = true;
        bool            missing = false;
        float           depth_offset = 0.0f;
        bool            runtime_flip_horizontal = false;
        bool            runtime_flip_vertical = false;
        float           runtime_rotation_degrees = 0.0f;
        Asset*          owner = nullptr;
        struct UpdateKey {
                anchor_points::GridMaterialization grid_policy = anchor_points::GridMaterialization::None;
                std::uint64_t camera_state_version = 0;
                bool initialized = false;

                bool matches(anchor_points::GridMaterialization grid,
                             std::uint64_t camera_version) const {
                        return initialized &&
                               grid_policy == grid &&
                               camera_state_version == camera_version;
                }

                void set(anchor_points::GridMaterialization grid,
                         std::uint64_t camera_version) {
                        grid_policy = grid;
                        camera_state_version = camera_version;
                        initialized = true;
                }
        } last_update_key_;

        void update(anchor_points::GridMaterialization grid_policy,
                    bool force_recompute = false,
                    const DisplacedAssetAnchorPoint* override_anchor = nullptr);
    };

    // A single source of truth for inputs that influence resolved anchor output
    // (world coordinates and cached screen projection).
    struct AnchorBasisSignature {
        int   world_x = 0;
        int   world_y = 0;
        int   world_z = 0;
        int   frame_index = 0;
        int   variant_index = 0;
        bool  flipped = false;
        bool  render_flip_horizontal = false;
        bool  render_flip_vertical = false;
        float render_rotation_degrees = 0.0f;
        float remainder_scale = 1.0f;       // runtime scale applied to rendered frame geometry
        float perspective_scale = 1.0f;     // depth-based scaling from the grid/camera
        float world_z_offset = 0.0f;        // render depth offset used by cached anchor screen projection
        float directional_heading_radians = 0.0f; // runtime directional heading used by oval anchor mappings
        bool  directional_heading_valid = false;
        float directional_target_world_x = 0.0f; // runtime pointer target used by oval anchor mappings
        float directional_target_world_z = 0.0f;
        bool  directional_target_valid = false;
        int   resolution_layer = 0;         // grid resolution used for anchor materialization
        std::uint64_t camera_state_version = 0; // camera snapshot that produced cached anchor placement
    };

    enum class AnchorResolveMode {
        Cached,
        ForceRecompute
    };

    AnchorPoint* get_anchor_point(const std::string& name);
    std::optional<std::string> anchor_name_for_index(std::size_t index) const;
    std::optional<AnchorPoint> anchor_state(const std::string& name,
                                            anchor_points::GridMaterialization grid_policy = anchor_points::GridMaterialization::None,
                                            AnchorResolveMode resolve_mode = AnchorResolveMode::Cached);
    void assert_unique_anchor_names_for_frame() const;
    AnchorPoint& resolve_anchor_point_entry(std::size_t index,
                                            anchor_points::GridMaterialization grid_policy,
                                            const DisplacedAssetAnchorPoint* frame_anchor,
                                            AnchorResolveMode resolve_mode);
    void apply_anchor_runtime_state(AnchorPoint& resolved,
                                    const AnchorHandle& handle,
                                    const DisplacedAssetAnchorPoint* frame_anchor) const;
    void refresh_anchor_point_cache_from_frame();
    void refresh_runtime_box_cache_from_frame();
    void refresh_runtime_floor_boxes_cache();
    void mark_anchors_dirty();
    void invalidate_anchor_registry();
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
    bool enforce_default_controller_animation_ = true;
    bool anchor_hidden_ = false;
    bool selected = false;
    void set_provisional_grid_point(const world::GridPoint& point);
    void set_provisional_grid_point(int world_x, int world_y, int world_z, int resolution_layer);
    GridPoint provisional_grid_point_ = GridPoint::make_virtual(0, 0, 0, 0);
    // Non-owning pointer to the current authoritative grid point. When detached from
    // WorldGrid, this falls back to provisional_grid_point_ until reattachment.
    GridPoint* grid_point_ = &provisional_grid_point_;
    void set_flip();

    float frame_progress = 0.0f;
    Assets* assets_ = nullptr;
    std::unique_ptr<AssetController>   controller_;
    std::vector<Asset*> children_;



    std::optional<TilingInfo> tiling_info_{};
    float size_variation_sample_ = 0.0f;
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
    bool         composite_dirty_   = true;
    SDL_Rect     composite_rect_    = {0, 0, 0, 0};
    float        composite_scale_   = 1.0f;
    float        world_z_offset_    = 0.0f;
    float        render_anchor_offset_x_ = 0.0f;
    float        render_anchor_offset_y_ = 0.0f;
    float        render_anchor_offset_z_ = 0.0f;
    bool         anchor_sprite_transform_override_active_ = false;
    SDL_FlipMode anchor_sprite_transform_override_flip_ = SDL_FLIP_NONE;
    double       anchor_sprite_transform_override_angle_degrees_ = 0.0;
    bool         anchor_perspective_override_active_ = false;
    float        anchor_perspective_override_scale_ = 1.0f;
    int          anchor_perspective_override_resolution_layer_ = 0;
    float        directional_heading_radians_ = 0.0f;
    bool         directional_heading_valid_ = false;
    float        directional_target_world_x_ = 0.0f;
    float        directional_target_world_z_ = 0.0f;
    bool         directional_target_valid_ = false;
    bool         mesh_dirty_        = true;

    void initialize_anchor_registry_from_animations();
    AnchorHandle* find_anchor_handle(const std::string& name);

    std::vector<AnchorHandle> anchor_handles_;
    std::vector<AnchorPoint> anchor_points_;
    std::unordered_map<std::string, std::size_t> anchor_name_to_index_;
    std::vector<RuntimeBoxVolume> current_hit_box_volumes_;
    std::vector<RuntimeBoxVolume> current_attack_box_volumes_;
    std::vector<RuntimeBoxVolume> current_impassable_box_volumes_;
    std::vector<RuntimeImpassableShape> current_impassable_shapes_;
    std::vector<RuntimeFloorBox> floor_boxes_;
    std::unordered_map<std::string, std::size_t> runtime_hit_box_lookup_;
    std::unordered_map<std::string, std::size_t> runtime_attack_box_lookup_;
    bool anchors_initialized_ = false;

    std::uint64_t anchor_world_revision_ = 1;
    AnchorBasisSignature last_anchor_basis_signature_{};
    bool anchor_basis_initialized_ = false;

    void refresh_filter_tags();

    std::string filter_type_tag_;
    std::string filter_method_tag_;

};

#endif
