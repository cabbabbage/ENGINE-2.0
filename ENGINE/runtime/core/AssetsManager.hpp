#pragma once

#include "rendering/render/warped_screen_grid.hpp"
#include "assets/asset/asset_library.hpp"
#include "core/popup_manager.hpp"
#include "core/game_runtime_context.hpp"
#include "core/runtime_game_config.hpp"
#include "utils/map_grid_settings.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <utility>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "gameplay/map_generation/room.hpp"
#include "gameplay/world/world_grid.hpp"
#include "assets/asset/Asset.hpp"
#include "gameplay/world/grid_point.hpp"
#include "core/manifest/map_data.hpp"
#include "runtime_world_context.hpp"

class Asset;
class OpenGLRuntimeRenderer;
struct SDL_Renderer;
class CurrentRoomFinder;
class Room;
class Input;
class DevControls;
class AssetInfo;
class TaskEditor;
namespace animation_editor {
class AnimationDocument;
class PreviewProvider;
}
namespace devmode::core {
class ManifestStore;
}

enum class FrameEditorLaunchMode {
    Movement,
    AnchorPoints
};

class Assets {
public:
    enum class LiveDynamicMode {
        BoundaryArea = 0,
        InheritedMap = 1,
    };

    enum class DevGridOverlayKind {
        FloorMouseCentered,
        XYPlaneAtAssetDepth,
        FloorCenteredOnSelectedPoint,
    };

    struct DevGridOverlayContext {
        DevGridOverlayKind kind = DevGridOverlayKind::FloorMouseCentered;
        float target_world_z = 0.0f;
        SDL_FPoint exact_floor_xz{0.0f, 0.0f};
        SDL_Point snapped_floor_xz{0, 0};
        bool has_selected_point_center = false;
    };

    struct DevFloorProjectionMarker {
        enum class Shape {
            Dot = 0,
            Crosshair = 1,
        };

        SDL_FPoint floor_world_xz{0.0f, 0.0f};
        SDL_Color color{255, 255, 255, 200};
        Shape shape = Shape::Dot;
        int pixel_size = 3;
        int crosshair_radius = 0;
        bool emphasized = false;
    };

    class WorldMutationBatch {
    public:
        explicit WorldMutationBatch(Assets* owner = nullptr) : owner_(owner) {}

        void mark_for_deletion(Asset* asset);
        void set_pre_commit_save(std::function<bool()> save_cb) { pre_commit_save_ = std::move(save_cb); }
        bool commit();
        bool has_mutations() const { return !staged_removals_.empty(); }

    private:
        friend class Assets;
        Assets* owner_ = nullptr;
        std::vector<Asset*> staged_removals_;
        std::unordered_set<Asset*> staged_lookup_;
        std::function<bool()> pre_commit_save_;
    };

    Assets(AssetLibrary& library,
           Asset*,
           std::shared_ptr<RuntimeWorldContext> world_context,
           int screen_width,
           int screen_height,
           int screen_center_x,
           int screen_center_z,
           int map_radius,
           SDL_Renderer* renderer,
           SDL_Window* window,
           const std::string& map_id,
           const nlohmann::json& map_manifest,
           std::string content_root = {},
           world::WorldGrid&& world_grid = world::WorldGrid{});
    ~Assets();

    nlohmann::json save_current_room(std::string room_name);
    void update(const Input& input);
    void set_dev_mode(bool mode);
    bool run_exit_save_sequence(const std::string& reason);
    void set_force_high_quality_rendering(bool enable);
    bool force_high_quality_rendering() const { return force_high_quality_rendering_; }
    void set_render_suppressed(bool suppressed);
    void set_input(Input* m);
    Input* get_input() const { return input; }
    Asset* find_asset_by_name(const std::string& name) const;
    Asset* find_asset_by_stable_id(const std::string& id) const;
    bool contains_asset(const Asset* asset) const;

    const std::vector<Asset*>& get_selected_assets() const;
    const std::vector<Asset*>& get_highlighted_assets() const;
    Asset* get_hovered_asset() const;

    const std::vector<Asset*>& getActive() const;
    const std::vector<Asset*>& getFilteredActiveAssets() const;
    const std::unordered_set<Asset*>& filtered_active_asset_membership() const { return filtered_active_asset_membership_; }
    using ActiveTraversalEntry = WarpedScreenGrid::VisibleTraversalEntry;
    struct FrameCollisionEntry {
        const Asset* asset = nullptr;
        Area area{"impassable"};
        SDL_Point world_center{0, 0};
        world::GridPoint bottom_middle = world::GridPoint::make_virtual(0, 0, 0, 0);
        std::string canonical_type;
    };
    const std::vector<ActiveTraversalEntry>& active_traversal() const { return camera_.visible_traversal_entries(); }
    const std::vector<FrameCollisionEntry>& frame_collision_entries() const { return frame_collision_entries_; }
    const std::vector<Asset*>& getActiveRaw() const { return active_assets; }
    std::vector<Asset*>& mutable_filtered_active_assets() { return filtered_active_assets; }
    WarpedScreenGrid& getView() { return camera_; }
    const WarpedScreenGrid& getView() const { return camera_; }
    void query_impassable_entries(const Asset& self,
                                  int search_radius,
                                  std::vector<const FrameCollisionEntry*>& out) const;
    int max_impassable_query_radius() const;
    void mark_collision_context_dirty() {
        frame_collision_context_dirty_ = true;
        frame_collision_query_cache_.clear();
    }

    float frame_delta_seconds() const { return last_frame_dt_seconds_; }
    float frame_delta_seconds_clamped() const;
    std::uint32_t frame_id() const { return frame_id_; }
    bool has_pending_initial_rebuild() const { return pending_initial_rebuild_; }
    std::size_t last_runtime_convergence_iterations() const { return last_runtime_convergence_stats_.iterations; }
    std::size_t last_runtime_convergence_traversal_refreshes() const { return last_runtime_convergence_stats_.traversal_refresh_count; }
    std::size_t last_runtime_convergence_waves() const { return last_runtime_convergence_stats_.wave_count; }
    std::size_t last_runtime_convergence_children_updated() const { return last_runtime_convergence_stats_.children_updated; }
    bool last_runtime_convergence_converged() const { return last_runtime_convergence_stats_.converged; }
    std::uint64_t visibility_fail_open_activation_count() const { return visibility_fail_open_activation_count_; }

    void render_overlays(SDL_Renderer* renderer, SDL_Texture* overlay_target = nullptr);
    SDL_Renderer* renderer() const;
    void toggle_asset_library();
    void open_asset_library();
    void close_asset_library();
    bool is_asset_library_open() const;
    void toggle_room_config();
    void close_room_config();
    bool is_room_config_open() const;

    std::shared_ptr<AssetInfo> consume_selected_asset_from_library();
    void open_asset_info_editor(const std::shared_ptr<AssetInfo>& info);
    void open_asset_info_editor_for_asset(Asset* a);
    void open_animation_editor_for_asset(const std::shared_ptr<AssetInfo>& info);
    void close_asset_info_editor();
    bool consume_escape_for_asset_editor_stack();
    bool is_asset_info_editor_open() const;

    void clear_editor_selection();
    void handle_sdl_event(const SDL_Event& e);
    void finalize_asset_drag(Asset* a, const std::shared_ptr<AssetInfo>& info);
    void on_camera_settings_changed();
    void reload_camera_settings();
    void apply_camera_runtime_settings();
    void force_camera_view_refresh();
    bool fog_visible() const;
    bool boundary_assets_visible() const;
    bool live_dynamic_assets_visible() const;
    float boundary_min_visible_screen_ratio() const;
    void set_boundary_min_visible_screen_ratio(float value);
    std::pair<int, int> camera_height_bounds_px() const;
    void set_camera_height_bounds_px(int min_value, int max_value);
    void sync_camera_settings_to_map_info_json();
    // Force the camera to refresh from current room settings on next update.
    void mark_camera_dirty();

    void focus_camera_on_asset(Asset* a, double height_factor = 0.8, int duration_steps = 25);

    void begin_frame_editor_session(Asset* asset,
                                    std::shared_ptr<animation_editor::AnimationDocument> document,
                                    std::shared_ptr<animation_editor::PreviewProvider> preview,
                                    const std::string& animation_id,
                                    FrameEditorLaunchMode launch_mode,
                                    std::function<void(const std::string&)> on_host_closed);

    void log_asset_movement(Asset* asset, const world::GridPoint& previous, const world::GridPoint& current);
    std::uint32_t current_frame_id() const { return frame_id_; }

    devmode::core::ManifestStore* manifest_store();
    const devmode::core::ManifestStore* manifest_store() const;
    void notify_spawn_group_config_changed(const nlohmann::json& entry);
    void notify_spawn_group_removed(const std::string& spawn_id);
    void show_dev_notice(const std::string& message, Uint32 duration_ms = 2000);
    void notify_camera_activity(bool active);

    void set_dev_grid_overlay_callback(std::function<void()> cb) { dev_grid_overlay_callback_ = cb; }

    void set_editor_current_room(Room* room);

    Room* current_room() { return current_room_; }
    const Room* current_room() const { return current_room_; }
    std::vector<const Room::NamedArea*> current_room_trigger_areas() const;

    nlohmann::json& map_info_json() { return map_info_json_; }
    const nlohmann::json& map_info_json() const { return map_info_json_; }
    bool mutate_map_data(const std::function<bool(manifest::MapData&)>& mutator);
    void mark_map_data_dirty();
    bool map_data_dirty() const { return map_data_dirty_; }
    void clear_map_data_dirty() { map_data_dirty_ = false; }
    // Capture the current in-memory room state (including spawn groups) into map_info_json_.
    void snapshot_rooms_to_map_info();
    const std::string& map_path() const { return map_path_; }
    const std::string& map_id() const { return map_id_; }
    world::WorldGrid& world_grid() { return world_grid_; }
    const world::WorldGrid& world_grid() const { return world_grid_; }
    world::GridPoint resolve_floor_world_point(SDL_Point world_pos, int resolution_layer = -1) const;

    // Suspend/reactivate assets outside the grid for shared binding helper.
    std::unique_ptr<Asset> extract_asset(Asset* asset);
    Asset* attach_asset(std::unique_ptr<Asset> asset, int world_z = 0, int resolution_layer = -1);
    std::unique_ptr<Asset> create_unattached_asset(const std::string& name, SDL_Point world_pos);

    void persist_map_info_json();

    AssetLibrary& library();
    const AssetLibrary& library() const;

    void ensure_light_textures_loaded(Asset* asset);
    std::vector<Room*>& rooms();
    const std::vector<Room*>& rooms() const;
    void notify_rooms_changed();
    std::size_t rooms_generation() const;
    RuntimeWorldContext* runtime_world_context();
    const RuntimeWorldContext* runtime_world_context() const;
    runtime::config::RuntimeGameConfig& runtime_game_config();
    const runtime::config::RuntimeGameConfig& runtime_game_config() const;
    const runtime::context::GameRuntimeContext& game_context() const { return game_context_; }
    runtime::context::GameRuntimeContext& mutable_game_context() { return game_context_; }

    void refresh_active_asset_lists();
    void refresh_filtered_active_assets();
    void mark_active_assets_dirty();
    void initialize_active_assets(const world::GridPoint& center);
    std::uint64_t dev_active_state_version() const { return dev_active_state_version_; }


    void apply_map_grid_settings(const MapGridSettings& settings, bool persist_json = true);
    const MapGridSettings& map_grid_settings() const { return map_grid_settings_; }
    bool dev_grid_overlay_enabled() const;
    int dev_grid_overlay_cell_size_px() const;
    DevGridOverlayContext dev_grid_overlay_context() const;
    std::vector<DevFloorProjectionMarker> dev_floor_projection_markers();

    std::optional<Asset::TilingInfo> compute_tiling_for_asset(const Asset* asset) const;

    bool should_run_runtime_updates() const;
    bool should_render_runtime_lighting() const;
    void set_camera_settings_panel_active(bool active);
    bool is_dev_mode() const { return dev_mode; }
    bool is_frame_editor_target_active(const Asset* asset) const;
    bool should_advance_animation_for(const Asset* asset) const;
    void set_focus_filter(Asset* asset, const std::string& spawn_id);
    void clear_focus_filter();
    bool focus_filter_active() const { return focus_filter_active_; }
    bool is_asset_in_focus_filter(const Asset* asset) const;
    bool is_spawn_id_in_focus_filter(const std::string& spawn_id) const;


    std::vector<Asset*> all;
    Asset* player = nullptr;

    Asset* spawn_asset(const std::string& name, SDL_Point world_pos);

    void rebuild_from_grid_state();

    const std::vector<world::Chunk*>& active_chunks() const { return world_grid_.active_chunks(); }

    bool has_pending_dev_work(bool include_animation_plans = true) const;
    bool should_step_dev_frame(const Input& input) const;
    void touch_last_frame_counter();
    bool process_pending_removals();
    std::size_t delete_assets_for_spawn_group(const std::string& spawn_id);
    std::size_t delete_assets_for_spawn_groups(const std::vector<std::string>& spawn_ids);
    WorldMutationBatch begin_world_mutation_batch();
    void set_output_dimensions(int width, int height);
    std::optional<SDL_Point> opengl_postprocess_target_size() const;

    void test_sync_live_dynamic_assets_for_bounds(const world::GridBounds& bounds);

private:
    void save_map_info_json();
    void hydrate_map_info_sections();
    void load_camera_settings_from_json();
    void write_camera_settings_to_json();
    void load_runtime_game_config_from_json();
    void write_runtime_game_config_to_json();
    void schedule_removal(Asset* a);
    std::vector<Asset*> collect_removal_closure(const std::vector<Asset*>& roots) const;
    std::size_t delete_assets_runtime(const std::vector<Asset*>& assets_to_delete);
    bool process_removals();
    bool apply_world_mutation_batch(WorldMutationBatch& batch);
    void addAsset(const std::string& name, SDL_Point g);
    void update_filtered_active_assets();
    bool asset_matches_focus_filter(const Asset* asset) const;
    void rebuild_focus_filter_closure();
    void mark_focus_filter_closure_dirty();
    void ensure_dev_controls();
    void sync_dev_controls_current_room(Room* room, bool force_refresh = false);
    void reset_dev_controls_current_room_cache();
    void log_camera_fog_state(const char* label) const;

    friend class Asset;

    CurrentRoomFinder* finder_ = nullptr;
    Input* input = nullptr;
    DevControls* dev_controls_ = nullptr;
    Room* dev_controls_last_room_ = nullptr;
    std::unique_ptr<TaskEditor> task_editor_;
    bool screenshot_capture_pending_ = false;
    std::string latest_screenshot_relative_path_;
    Uint32 screenshot_create_task_start_ticks_ = 0;
    SDL_Rect screenshot_create_task_button_rect_{};
    PopupManager popup_manager_;
    SDL_Window* app_window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    WarpedScreenGrid camera_;
    std::unique_ptr<OpenGLRuntimeRenderer> opengl_renderer_;
    int screen_width;
    int screen_height;
    int delta_x_ = 0;
    int delta_z_ = 0;
    std::vector<Asset*> active_assets;
    std::vector<Asset*> filtered_active_assets;
    std::unordered_set<Asset*> filtered_active_asset_membership_;
    std::shared_ptr<RuntimeWorldContext> world_context_;
    Room* current_room_ = nullptr;
    bool dev_mode = false;
    bool camera_settings_dirty_ = false;
    bool camera_settings_panel_active_ = false;
    bool suppress_render_ = false;

    bool suppress_dev_renderer_ = false;
    bool force_high_quality_rendering_ = false;
    bool movement_debug_enabled_ = false;
    bool movement_debug_visible_ = true;
    bool anchor_point_debug_enabled_ = false;
    bool asset_boundary_box_display_enabled_ = false;
    world::WorldGrid world_grid_{};
    struct FrameCollisionQueryKey {
        std::uint64_t context_version = 0;
        const Asset* self_asset = nullptr;
        int self_world_x = 0;
        int self_world_z = 0;
        int radius = 0;

        bool operator==(const FrameCollisionQueryKey& other) const {
            return context_version == other.context_version &&
                   self_asset == other.self_asset &&
                   self_world_x == other.self_world_x &&
                   self_world_z == other.self_world_z &&
                   radius == other.radius;
        }
    };
    struct FrameCollisionQueryKeyHash {
        std::size_t operator()(const FrameCollisionQueryKey& key) const {
            std::size_t seed = std::hash<std::uint64_t>{}(key.context_version);
            auto mix = [&seed](std::size_t value) {
                seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            };
            mix(std::hash<const void*>{}(static_cast<const void*>(key.self_asset)));
            mix(std::hash<int>{}(key.self_world_x));
            mix(std::hash<int>{}(key.self_world_z));
            mix(std::hash<int>{}(key.radius));
            return seed;
        }
    };
    mutable std::vector<FrameCollisionEntry> frame_collision_entries_;
    mutable std::unordered_map<std::uint64_t, std::vector<const FrameCollisionEntry*>> frame_collision_index_;
    mutable std::unordered_map<FrameCollisionQueryKey,
                               std::vector<const FrameCollisionEntry*>,
                               FrameCollisionQueryKeyHash> frame_collision_query_cache_;
    mutable std::uint64_t frame_collision_context_version_ = 1;
    mutable std::uint32_t frame_collision_context_frame_id_ = 0;
    mutable bool frame_collision_context_dirty_ = true;
    std::vector<Asset*> removal_queue;
    std::mutex removal_queue_mutex_;
    std::vector<Asset*> non_player_update_buffer_;
    std::atomic<bool> non_player_update_buffer_dirty_{true};

    float      last_frame_dt_seconds_   = 1.0f / 60.0f;
    double     perf_counter_frequency_  = 0.0;
    std::uint64_t last_frame_counter_   = 0;

    AssetLibrary& library_;
    std::string map_id_;
    std::string map_path_;
    nlohmann::json map_info_json_;
    bool map_data_dirty_ = false;
    bool exit_save_sequence_ran_ = false;
    bool exit_save_sequence_ok_ = true;
    std::atomic<bool> active_assets_dirty_{true};
    MapGridSettings map_grid_settings_{};
    std::optional<float> last_audio_effect_max_distance_{};
    float max_asset_height_world_ = 0.0f;
    float max_asset_width_world_  = 0.0f;
    float cached_height_level_      = 0.0f;
    bool  max_asset_dimensions_dirty_ = true;
    float boundary_min_visible_screen_ratio_ = 0.015f;
    int camera_height_min_px_ = 1;
    int camera_height_max_px_ = 100000;
    int map_radius_world_ = 0;
    struct AssetDimensionCache {
        float width = 0.0f;
        float height = 0.0f;
    };
    struct RuntimeConvergencePassResult {
        bool any_change = false;
        bool needs_repass = false;
        bool needs_traversal_refresh = false;
        std::size_t wave_count = 0;
        std::size_t children_considered = 0;
        std::size_t children_updated = 0;
    };
    struct RuntimeConvergenceFrameStats {
        std::size_t iterations = 0;
        std::size_t traversal_refresh_count = 0;
        std::size_t wave_count = 0;
        std::size_t children_considered = 0;
        std::size_t children_updated = 0;
        bool converged = false;
        double stage_ms = 0.0;
        double pass_ms = 0.0;
        double refresh_ms = 0.0;
    };
    std::unordered_map<Asset*, AssetDimensionCache> asset_dimension_cache_;
    std::vector<Asset*> asset_dimension_update_queue_;
    std::unordered_set<Asset*> asset_dimension_update_lookup_;
    struct RuntimeTraversalState {
        std::uint64_t pending_anchor_invalidation_version = 0;
        std::uint64_t processed_anchor_invalidation_version = 0;
        std::uint64_t processed_anchor_revision = 0;
        std::uint64_t processed_camera_state_version = 0;
        int processed_frame_index = std::numeric_limits<int>::min();
        Asset::RuntimeImpassableGeometrySignature processed_impassable_signature{};
        bool processed_impassable_signature_initialized = false;
        std::uint32_t non_player_update_visit_epoch = 0;
    };
    std::unordered_map<Asset*, RuntimeTraversalState> runtime_traversal_state_;
    std::uint64_t anchor_invalidation_version_counter_ = 1;
    std::uint32_t last_audio_engine_update_frame_id_ = 0;
    Asset* max_asset_width_holder_ = nullptr;
    Asset* max_asset_height_holder_ = nullptr;
    std::uint64_t active_assets_generation_ = 1;
    std::uint64_t visibility_fail_open_activation_count_ = 0;
    std::uint32_t frame_id_ = 0;
    std::uint32_t non_player_update_visit_epoch_ = 0;
    std::size_t startup_non_player_update_cursor_ = 0;
    std::size_t startup_runtime_pass_cursor_ = 0;
    std::uint32_t last_active_rebuild_frame_id_ = 0;
    bool visibility_fail_open_used_last_rebuild_ = false;
    std::uint32_t last_grid_rebuild_frame_ = 0;
    std::uint32_t last_runtime_convergence_warning_frame_id_ = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t frame_rebuild_metrics_frame_ = 0;
    std::uint32_t frame_rebuild_request_count_ = 0;
    std::uint32_t frame_rebuild_execution_count_ = 0;
    bool frame_rebuild_metrics_initialized_ = false;
    RuntimeConvergenceFrameStats last_runtime_convergence_stats_{};

    bool pending_initial_rebuild_ = false;
    bool post_runtime_traversal_refresh_pending_ = false;
    bool logged_initial_rebuild_warning_ = false;
    bool grid_dirty_ = true;
    bool camera_view_dirty_ = true;
    world::GridPoint last_camera_center_for_grid_ = world::GridPoint::make_virtual(0, 0, 0, 0);
    double last_camera_scale_for_grid_ = 0.0;
    double last_camera_pitch_for_grid_ = 0.0;
    std::uint64_t last_camera_projection_state_version_for_grid_ = 0;

    struct GridMovementCommand {
        Asset* asset = nullptr;
        world::GridPoint previous = world::GridPoint::make_virtual(0, 0, 0, 0);
        world::GridPoint current   = world::GridPoint::make_virtual(0, 0, 0, 0);
    };

    void track_asset_for_grid(Asset* asset);
    void reset_frame_rebuild_stage();
    void note_frame_rebuild_request();
    bool run_frame_rebuild_stage();
    bool maybe_rebuild_world_grid();
    void rebuild_world_grid_and_active_assets(const world::GridPoint& current_center,
                                              double current_scale,
                                              double current_pitch,
                                              std::uint64_t current_projection_version,
                                              bool allow_live_dynamic_sync = true);
    void mark_grid_dirty();
    void untrack_asset_for_grid(Asset* asset);
    void register_pending_static_assets();
    void rebuild_all_assets_from_grid();
    void rebuild_active_from_screen_grid();
    void migrate_live_dynamic_spawn_config();
    void rebuild_live_dynamic_selectors();
    void sync_live_dynamic_assets_to_render_bounds(const world::GridBounds& render_bounds);
    world::GridBounds runtime_work_bounds_from_render_bounds(const world::GridBounds& render_bounds);
    bool should_run_live_dynamic_sync_for_bounds(const world::GridBounds& work_bounds, bool allow_live_dynamic_sync);
    void clear_live_dynamic_assets();
    std::size_t delete_live_dynamic_assets_now(const std::vector<Asset*>& assets_to_delete);

    std::vector<Asset*> moving_assets_for_grid_;
    std::vector<Asset*> pending_static_grid_registration_;
    std::vector<GridMovementCommand> movement_commands_buffer_;
    std::vector<Asset*> grid_registration_buffer_;

    void touch_dev_active_state_version();

    std::uint64_t dev_active_state_version_ = 1;

    std::function<void()> dev_grid_overlay_callback_;

    void rebuild_non_player_update_buffer_if_needed();
    void refresh_visible_asset_scaling_only();
    void run_idle_frame_pipeline(const Input& input);
    void run_world_update_stage(const Input& input, bool& room_changed, bool& player_moved);
    void run_visibility_build_stage();
    void run_post_flush_traversal_refresh_once();
    void run_runtime_effects_stage(bool include_audio_update = true);
    void sync_dev_controls_for_frame(const Input& input);
    void refresh_filtered_active_assets_if_needed();
    void render_runtime_frame();
    SDL_Texture* prepare_runtime_ui_overlay_texture();
    void destroy_runtime_ui_overlay_texture();
    void finalize_dev_frame_state();
    void mark_anchor_basis_dirty(Asset* asset);
    void mark_anchor_bases_dirty_for_active_assets();
    std::uint64_t next_anchor_invalidation_version();
    RuntimeConvergencePassResult run_active_runtime_single_pass(bool include_audio_update = true);
    void run_active_runtime_single_pass_for_asset(Asset* asset,
                                                  const SDL_Point& camera_focus,
                                                  std::uint64_t camera_state_version,
                                                  float camera_anchor_world_z,
                                                  float depth_axis_sign);
    void run_camera_trap_escape_pass();
    bool capture_screenshot_to_root(SDL_Renderer* renderer, std::string& out_relative_path);
    bool screenshot_create_task_button_active(Uint32 now_ticks) const;
    void render_screenshot_create_task_button(SDL_Renderer* renderer, Uint32 now_ticks);
    bool handle_screenshot_create_task_button_event(const SDL_Event& e, Uint32 now_ticks);
    void rebuild_frame_collision_context() const;
    void update_active_assets(const world::GridPoint& center);
    bool asset_bounds_in_screen_space(const Asset* asset, SDL_FRect& out_rect) const;
    void update_max_asset_dimensions();
    void invalidate_max_asset_dimensions();
    void queue_asset_dimension_update(Asset* asset);
    void remove_asset_dimension_cache(Asset* asset);
    void rebuild_asset_dimension_cache(float camera_scale);
    bool compute_asset_dimension_cache(const Asset* asset, float camera_scale, AssetDimensionCache& out) const;
    void finalize_max_asset_dimensions(float max_width, float max_height);
    world::GridBounds screen_world_rect() const;
    int audio_effect_max_distance_world() const;

    void mark_non_player_update_buffer_dirty() {
        non_player_update_buffer_dirty_.store(true, std::memory_order_release);
    }

private:
    world::GridPoint last_known_player_pos_ = world::GridPoint::make_virtual(0, 0, 0, 0);
    bool      last_player_pos_valid_ = false;

    std::vector<SDL_Rect> culled_debug_rects_;
    std::uint64_t filtered_active_assets_source_generation_ = 0;
    std::uint64_t filtered_active_assets_filter_version_ = 0;
    bool needs_filtered_active_refresh_ = true;
    bool last_dev_controls_enabled_ = false;
    std::uint64_t last_dev_filter_state_version_ = 0;
    std::uint64_t last_camera_state_version_for_dev_ = 0;
    std::uint64_t last_dev_active_state_version_snapshot_ = 0;
    bool dev_frame_initialized_ = false;
    bool focus_filter_active_ = false;
    Asset* focus_filter_asset_ = nullptr;
    std::string focus_filter_spawn_id_;
    std::unordered_set<const Asset*> focus_filter_closure_;
    bool focus_filter_closure_dirty_ = true;
    std::uint64_t focus_filter_version_ = 0;
    SDL_Texture* runtime_ui_overlay_texture_ = nullptr;
    int runtime_ui_overlay_width_ = 0;
    int runtime_ui_overlay_height_ = 0;
    runtime::context::GameRuntimeContext game_context_{};

    struct LiveDynamicCandidate {
        std::string asset_name;
        std::shared_ptr<AssetInfo> info;
        double weight = 0.0;
        bool is_null = false;
    };

    struct LiveDynamicSelector {
        std::string spawn_id;
        std::string display_name;
        int grid_resolution = 0;
        int jitter_px = 0;
        LiveDynamicMode mode = LiveDynamicMode::BoundaryArea;
        std::vector<LiveDynamicCandidate> candidates;
    };

    struct LiveDynamicPointKey {
        LiveDynamicMode mode = LiveDynamicMode::BoundaryArea;
        int grid_resolution = 0;
        int grid_x = 0;
        int grid_z = 0;
        std::string spawn_id;

        bool operator==(const LiveDynamicPointKey& other) const {
            return mode == other.mode &&
                   grid_resolution == other.grid_resolution &&
                   grid_x == other.grid_x &&
                   grid_z == other.grid_z &&
                   spawn_id == other.spawn_id;
        }
    };

    struct LiveDynamicPointKeyHash {
        std::size_t operator()(const LiveDynamicPointKey& key) const {
            std::size_t seed = std::hash<int>{}(static_cast<int>(key.mode));
            auto mix = [&seed](std::size_t value) {
                seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            };
            mix(std::hash<int>{}(key.grid_resolution));
            mix(std::hash<int>{}(key.grid_x));
            mix(std::hash<int>{}(key.grid_z));
            mix(std::hash<std::string>{}(key.spawn_id));
            return seed;
        }
    };

    std::vector<LiveDynamicSelector> live_dynamic_boundary_selectors_;
    std::vector<LiveDynamicSelector> live_dynamic_inherited_selectors_;
    std::unordered_map<Asset*, LiveDynamicPointKey> live_dynamic_asset_keys_;
    std::unordered_set<LiveDynamicPointKey, LiveDynamicPointKeyHash> live_dynamic_spawned_keys_;
    int live_dynamic_preload_margin_world_px_ = 192;
    int live_dynamic_despawn_margin_world_px_ = 256;
    std::size_t max_live_dynamic_scan_cells_per_selector_per_frame_ = 8192;
    std::size_t max_live_dynamic_new_spawns_per_frame_ = 8192;
    std::size_t max_total_live_dynamic_assets_ = 20000;
    std::uint32_t last_live_dynamic_guard_warning_frame_ = std::numeric_limits<std::uint32_t>::max();
    int live_dynamic_sync_min_camera_delta_world_px_ = 96;
    std::uint32_t live_dynamic_sync_min_interval_frames_ = 8;
    bool force_live_dynamic_sync_next_rebuild_ = true;
    int last_live_dynamic_sync_center_world_x_ = 0;
    int last_live_dynamic_sync_center_world_z_ = 0;
    std::uint32_t last_live_dynamic_sync_frame_ = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t last_work_bounds_clamp_warning_frame_ = std::numeric_limits<std::uint32_t>::max();
};
