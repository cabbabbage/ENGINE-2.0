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

namespace world { class WorldGrid; }

using world::GridPoint;

struct RenderObject {
    SDL_Texture* texture = nullptr;
    SDL_Rect screen_rect{};
    float world_anchor_x = std::numeric_limits<float>::quiet_NaN();
    float world_anchor_y = std::numeric_limits<float>::quiet_NaN();
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
    int atlas_w = 0;
    int atlas_h = 0;
    bool has_atlas_size = false;
    SDL_Texture* dimension_cache_texture = nullptr;
    std::array<SDL_Vertex, 4> cached_vertices{};
    std::array<int, 6> cached_indices{0, 1, 2, 0, 2, 3};
    SDL_FPoint cached_position{0.0f, 0.0f};
    float cached_world_z = 0.0f;
    float cached_scale = 0.0f;
    std::int64_t cached_position_key_x = 0;
    std::int64_t cached_position_key_y = 0;
    std::int64_t cached_world_z_key = 0;
    std::int64_t cached_scale_key = 0;
    bool cached_projection_key_valid = false;
    std::uint64_t cached_camera_state_version = 0;
    SDL_Texture* cached_mesh_texture = nullptr;
    bool has_cached_mesh = false;
    bool mesh_dirty = true;
    SDL_FPoint projection_anchor_uv{0.5f, 1.0f};
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
    enum class PerspectiveSource {
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
    double render_depth_bias_ = 0.0;


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
        int damage_amount = 0;
        std::string meta_json;
        std::array<RuntimeBoxPoint3, 8> world_points{};
        RuntimeBoxPoint3 centroid{};
        bool valid = false;
    };

    const std::vector<RuntimeBoxVolume>& current_hit_box_volumes() const { return current_hit_box_volumes_; }
    const std::vector<RuntimeBoxVolume>& current_attack_box_volumes() const { return current_attack_box_volumes_; }
    const RuntimeBoxVolume* find_hit_box_volume(const std::string& name) const;
    const RuntimeBoxVolume* find_attack_box_volume(const std::string& name) const;

    struct AnchorHandle {
        std::string     name;
        world::GridPoint* grid = nullptr;
        Vec2            world_exact_pos_2d{};
        float           world_exact_z = 0.0f;
        SDL_Point       world_px{0, 0};
        int             world_z = 0;
        float           world_depth = 0.0f;
        int             resolution_layer = 0;
        SDL_Point       source_texture_px{0, 0};
        SDL_FPoint      screen_px{0.0f, 0.0f};
        bool            has_canonical_texture_source = false;
        bool            dirty = true;
        bool            missing = false;
        int             depth_offset = 0;
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

        void update(anchor_points::GridMaterialization grid_policy, bool force_recompute = false);
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
    std::uint64_t composite_depth_cue_settings_version_ = 0;
    float        world_z_offset_    = 0.0f;
    float        render_anchor_offset_x_ = 0.0f;
    float        render_anchor_offset_y_ = 0.0f;
    float        render_anchor_offset_z_ = 0.0f;
    bool         anchor_sprite_transform_override_active_ = false;
    SDL_FlipMode anchor_sprite_transform_override_flip_ = SDL_FLIP_NONE;
    double       anchor_sprite_transform_override_angle_degrees_ = 0.0;
    bool         mesh_dirty_        = true;

    void initialize_anchor_registry_from_animations();
    AnchorHandle* find_anchor_handle(const std::string& name);

    std::vector<AnchorHandle> anchor_handles_;
    std::vector<AnchorPoint> anchor_points_;
    std::unordered_map<std::string, std::size_t> anchor_name_to_index_;
    std::vector<RuntimeBoxVolume> current_hit_box_volumes_;
    std::vector<RuntimeBoxVolume> current_attack_box_volumes_;
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
