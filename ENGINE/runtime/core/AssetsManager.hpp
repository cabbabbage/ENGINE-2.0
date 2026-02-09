#pragma once

#include "rendering/render/warped_screen_grid.hpp"
#include "assets/asset_library.hpp"
#include "core/popup_manager.hpp"
#include <SDL3/SDL.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "gameplay/map_generation/room.hpp"
#include "gameplay/world/world_grid.hpp"
#include "assets/Asset.hpp"
#include "gameplay/world/grid_point.hpp"

class Asset;
class SceneRenderer;
struct SDL_Renderer;
class CurrentRoomFinder;
class Room;
class Input;
class DevControls;
class AssetInfo;

class QuickTaskPopup;
namespace animation_editor {
class AnimationDocument;
class PreviewProvider;
}
namespace devmode::core {
class ManifestStore;
}

enum class FrameEditorLaunchMode {
    Movement,
    SyncChildren,
    AsyncChildren,
    AttackGeometry,
    HitGeometry
};

class Assets {
public:
    Assets(AssetLibrary& library,
           Asset*,
           std::vector<Room*> rooms,
           int screen_width,
           int screen_height,
           int screen_center_x,
           int screen_center_y,
           int map_radius,
           SDL_Renderer* renderer,
           const std::string& map_id,
           const nlohmann::json& map_manifest,
           std::string content_root = {},
           world::WorldGrid&& world_grid = world::WorldGrid{});
    ~Assets();

    nlohmann::json save_current_room(std::string room_name);
    void update(const Input& input);
    void set_dev_mode(bool mode);
    void set_force_high_quality_rendering(bool enable);
    bool force_high_quality_rendering() const { return force_high_quality_rendering_; }
    void set_render_suppressed(bool suppressed);
    void set_input(Input* m);
    Input* get_input() const { return input; }
    Asset* find_asset_by_name(const std::string& name) const;
    bool contains_asset(const Asset* asset) const;

    const std::vector<Asset*>& get_selected_assets() const;
    const std::vector<Asset*>& get_highlighted_assets() const;
    Asset* get_hovered_asset() const;

    const std::vector<Asset*>& getActive() const;
    const std::vector<Asset*>& getFilteredActiveAssets() const;
    const std::vector<world::GridPoint*>& active_points() const { return active_points_; }
    struct ActiveTraversalEntry {
        Asset* asset = nullptr;
        world::GridPoint* grid_point = nullptr;
        double depth_from_anchor = 0.0;
    };
    const std::vector<ActiveTraversalEntry>& active_traversal() const { return active_traversal_; }
    const std::vector<Asset*>& getActiveRaw() const { return active_assets; }
    std::vector<Asset*>& mutable_filtered_active_assets() { return filtered_active_assets; }
    WarpedScreenGrid& getView() { return camera_; }
    const WarpedScreenGrid& getView() const { return camera_; }

    float frame_delta_seconds() const { return last_frame_dt_seconds_; }

    void render_overlays(SDL_Renderer* renderer);
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
    bool is_asset_info_editor_open() const;

    void clear_editor_selection();
    void handle_sdl_event(const SDL_Event& e);
    void finalize_asset_drag(Asset* a, const std::shared_ptr<AssetInfo>& info);
    void on_camera_settings_changed();
    void reload_camera_settings();
    void apply_camera_runtime_settings();
    void force_camera_view_refresh();
    void set_depth_effects_enabled(bool enabled);
    bool depth_effects_enabled() const { return depth_effects_enabled_; }
    void set_movement_debug_enabled(bool enabled);
    bool movement_debug_enabled() const { return movement_debug_enabled_; }
    void set_movement_debug_visible(bool visible);
    bool movement_debug_visible() const { return movement_debug_visible_; }
    bool fog_visible() const;
    bool boundary_assets_visible() const;
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
    void invalidate_dynamic_boundary_system();

    void show_dev_notice(const std::string& message, Uint32 duration_ms = 2000);
    void notify_camera_activity(bool active);

    void set_dev_grid_overlay_callback(std::function<void()> cb) { dev_grid_overlay_callback_ = cb; }

    void set_editor_current_room(Room* room);

    Room* current_room() { return current_room_; }
    const Room* current_room() const { return current_room_; }
    std::vector<const Room::NamedArea*> current_room_trigger_areas() const;

    nlohmann::json& map_info_json() { return map_info_json_; }
    const nlohmann::json& map_info_json() const { return map_info_json_; }
    const std::string& map_path() const { return map_path_; }
    const std::string& map_id() const { return map_id_; }
    world::WorldGrid& world_grid() { return world_grid_; }
    const world::WorldGrid& world_grid() const { return world_grid_; }

    void persist_map_info_json();

    AssetLibrary& library();
    const AssetLibrary& library() const;

    void set_rooms(std::vector<Room*> rooms);
    void ensure_light_textures_loaded(Asset* asset);
    std::vector<Room*>& rooms();
    const std::vector<Room*>& rooms() const;
    void notify_rooms_changed();
    std::size_t rooms_generation() const { return rooms_generation_; }

    void refresh_active_asset_lists();
    void refresh_filtered_active_assets();
    void mark_active_assets_dirty();
    bool rebuild_active_assets_if_needed();
    void initialize_active_assets(const world::GridPoint& center);
    std::uint64_t dev_active_state_version() const { return dev_active_state_version_; }

    // Region tagging
    void classify_region(world::GridPoint& point);


    void apply_map_grid_settings(const MapGridSettings& settings, bool persist_json = true);
    int  map_grid_chunk_resolution() const;
    const MapGridSettings& map_grid_settings() const { return map_grid_settings_; }

    std::optional<Asset::TilingInfo> compute_tiling_for_asset(const Asset* asset) const;

    bool should_run_runtime_updates() const;
    bool is_dev_mode() const { return dev_mode; }


    std::vector<Asset*> all;
    Asset* player = nullptr;

    Asset* spawn_asset(const std::string& name, SDL_Point world_pos);
    void request_child_timeline_creation(Asset* parent);
    Asset* find_child_timeline_asset(const Asset* parent, int slot_index) const;

    void rebuild_from_grid_state();

    const std::vector<world::Chunk*>& active_chunks() const { return world_grid_.active_chunks(); }

    bool has_pending_dev_work(bool include_animation_plans = true) const;
    bool should_step_dev_frame(const Input& input) const;
    void touch_last_frame_counter();
    bool process_pending_removals();

private:
    void save_map_info_json();
    void hydrate_map_info_sections();
    void load_camera_settings_from_json();
    void write_camera_settings_to_json();
    void schedule_removal(Asset* a);

    bool process_removals();
    void addAsset(const std::string& name, SDL_Point g);
    void update_filtered_active_assets();
    void ensure_dev_controls();
    void update_scene_render_quality();
    int  saved_render_quality_percent() const;
    int  effective_render_quality_percent() const;
    void sync_dev_controls_current_room(Room* room, bool force_refresh = false);
    void reset_dev_controls_current_room_cache();
    void log_camera_fog_state(const char* label) const;

    friend class SceneRenderer;
    friend class Asset;

    CurrentRoomFinder* finder_ = nullptr;
    Input* input = nullptr;
    DevControls* dev_controls_ = nullptr;
    Room* dev_controls_last_room_ = nullptr;
    std::unique_ptr<QuickTaskPopup> quick_task_popup_;
    PopupManager popup_manager_;
    WarpedScreenGrid camera_;
    SceneRenderer* scene = nullptr;
    int screen_width;
    int screen_height;
    int dx = 0;
    int dy = 0;
    std::vector<Asset*> active_assets;
    std::vector<Asset*> filtered_active_assets;
    std::vector<Room*> rooms_;
    std::size_t rooms_generation_ = 0;
    Room* current_room_ = nullptr;
    bool dev_mode = false;
    bool camera_settings_dirty_ = false;
    bool suppress_render_ = false;

    bool suppress_dev_renderer_ = false;
    bool force_high_quality_rendering_ = false;
    bool depth_effects_enabled_ = true;
    bool movement_debug_enabled_ = false;
    bool movement_debug_visible_ = true;
    bool asset_boundary_box_display_enabled_ = false;
    world::WorldGrid world_grid_{};
    std::vector<world::GridPoint*> active_points_;
    std::vector<ActiveTraversalEntry> active_traversal_;
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
    std::atomic<bool> active_assets_dirty_{true};
    MapGridSettings map_grid_settings_{};
    std::unique_ptr<devmode::core::ManifestStore> manifest_store_fallback_;
    std::optional<float> last_audio_effect_max_distance_{};
    float max_asset_height_world_ = 0.0f;
    float max_asset_width_world_  = 0.0f;
    float cached_height_level_      = 0.0f;
    bool  max_asset_dimensions_dirty_ = true;
    struct AssetDimensionCache {
        float width = 0.0f;
        float height = 0.0f;
    };
    std::unordered_map<Asset*, AssetDimensionCache> asset_dimension_cache_;
    std::vector<Asset*> asset_dimension_update_queue_;
    std::unordered_set<Asset*> asset_dimension_update_lookup_;
    Asset* max_asset_width_holder_ = nullptr;
    Asset* max_asset_height_holder_ = nullptr;
    std::vector<Asset*> visible_candidate_buffer_;
    std::uint64_t active_candidate_generation_ = 0;
    std::uint32_t frame_id_ = 0;
    std::uint32_t last_active_rebuild_frame_id_ = 0;
    std::uint32_t last_grid_rebuild_frame_ = 0;

    bool pending_initial_rebuild_ = false;
    bool logged_initial_rebuild_warning_ = false;
    bool grid_dirty_ = true;
    bool camera_view_dirty_ = true;
    world::GridPoint last_camera_center_for_grid_ = world::GridPoint::make_virtual(0, 0, 0, 0);
    double last_camera_scale_for_grid_ = 0.0;
    double last_camera_pitch_for_grid_ = 0.0;

    struct GridMovementCommand {
        Asset* asset = nullptr;
        world::GridPoint previous = world::GridPoint::make_virtual(0, 0, 0, 0);
        world::GridPoint current   = world::GridPoint::make_virtual(0, 0, 0, 0);
    };

    void track_asset_for_grid(Asset* asset);
    bool maybe_rebuild_world_grid();
    void rebuild_world_grid_and_active_assets(const world::GridPoint& current_center,
                                              double current_scale,
                                              double current_pitch);
    void mark_grid_dirty();
    void untrack_asset_for_grid(Asset* asset);
    void register_pending_static_assets();
    void rebuild_all_assets_from_grid();
    void rebuild_active_from_screen_grid();

    std::vector<Asset*> moving_assets_for_grid_;
    std::vector<Asset*> pending_static_grid_registration_;
    std::vector<GridMovementCommand> movement_commands_buffer_;
    std::vector<Asset*> grid_registration_buffer_;

    void touch_dev_active_state_version();

    std::uint64_t dev_active_state_version_ = 1;
    std::uint64_t filtered_active_assets_hash_ = 0;

    std::function<void()> dev_grid_overlay_callback_;

    void rebuild_non_player_update_buffer_if_needed();
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

    void update_audio_camera_metrics();
    void mark_non_player_update_buffer_dirty() {
        non_player_update_buffer_dirty_.store(true, std::memory_order_release);
    }

private:
    world::GridPoint last_known_player_pos_ = world::GridPoint::make_virtual(0, 0, 0, 0);
    bool      last_player_pos_valid_ = false;

    std::vector<SDL_Rect> culled_debug_rects_;
    std::uint64_t filtered_active_assets_source_hash_ = 0;
    std::uint64_t filtered_active_assets_filter_version_ = 0;
    bool needs_filtered_active_refresh_ = true;
    bool last_dev_controls_enabled_ = false;
    std::uint64_t last_dev_filter_state_version_ = 0;
    std::uint64_t last_camera_state_version_for_dev_ = 0;
    std::uint64_t last_dev_active_state_version_snapshot_ = 0;
    bool dev_frame_initialized_ = false;
};
#include "utils/map_grid_settings.hpp"
