#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "devtools/dev_camera_controls.hpp"
#include "devtools/animation_source_navigation.hpp"
#include "devtools/core/dev_save_coordinator.hpp"
#include "devtools/room_anchor_tools_panel.hpp"
#include "devtools/room_box_tools_panel.hpp"
#include "devtools/room_movement_payload.hpp"
#include "devtools/room_oval_tools_panel.hpp"
#include "devtools/room_selection_filter_utils.hpp"
#include "assets/asset/anchor_point.hpp"
#include "animation/combat_geometry.hpp"
#include "utils/input.hpp"

class Asset;
class Input;
class Assets;
class AssetLibraryUI;
class AssetInfoUI;
class Area;
class RoomConfigurator;
class SpawnGroupConfig;
class AssetInfo;
class Room;
class WarpedScreenGrid;
namespace vibble::grid {
class Occupancy;
class Grid;
}
class BottomNavigationPanel;
class RoomAnchorToolsPanel;
class RoomMovementToolsPanel;
class RoomOvalToolsPanel;
class CandidateEditorPieGraphWidget;
class DockableCollapsible;
class DevFooterBar;
class DevControls;
namespace animation_update { struct AttackPayload; }
namespace devmode::room_config { class AttackPayloadEditor; }

namespace devmode::core {
class ManifestStore;
class SaveManager;
}

class RoomEditor {
public:
    RoomEditor(Assets* owner, int screen_w, int screen_h);
    ~RoomEditor();

    void set_input(Input* input);
    void set_player(Asset* player);
    void set_active_assets(std::vector<Asset*>& actives, std::uint64_t generation);
    void set_screen_dimensions(int width, int height);
    void set_current_room(Room* room, bool lock_room = false);
    void unlock_current_room();
    bool is_room_locked_for_edit() const { return room_locked_for_edit_; }
    void set_room_config_visible(bool visible);
    void set_shared_footer_bar(DevFooterBar* footer);
    void set_snap_to_grid_enabled(bool enabled);
    void set_header_visibility_callback(std::function<void(bool)> cb);
    void set_boundary_assets_panel_callback(std::function<void()> cb);
    void set_manifest_store(devmode::core::ManifestStore* store);
    void set_save_coordinator(devmode::core::DevSaveCoordinator* coordinator);
    void set_save_manager(devmode::core::SaveManager* manager);
    void set_map_dirty_callback(std::function<void(devmode::core::DevSaveCoordinator::Priority)> cb) { mark_map_dirty_callback_ = std::move(cb); }

    void set_enabled(bool enabled, bool preserve_camera_state = false);
    bool is_enabled() const { return enabled_; }
    bool is_anchor_edit_mode_active() const;
    bool is_light_edit_mode_active() const;
    bool is_asset_stack_editor_active() const;
    void set_room_trail_nav_visibility(bool visible);

    void update(const Input& input);
    void update_ui(const Input& input);
    bool handle_sdl_event(const SDL_Event& event);
    bool is_room_panel_blocking_point(int x, int y) const;
    bool is_room_ui_blocking_point(int x, int y) const;
    bool is_shift_key_down() const;
    void set_camera_settings_lock(bool active);
    void apply_camera_settings_lock(WarpedScreenGrid& cam);
    SDL_Point camera_lock_target() const;
    void render_overlays(SDL_Renderer* renderer);
    void refresh_cursor_snap();

    void toggle_asset_library();
    void open_asset_library();
    void close_asset_library();
    bool is_asset_library_open() const;
    bool is_library_drag_active() const;

    std::shared_ptr<AssetInfo> consume_selected_asset_from_library();

    void open_asset_info_editor(const std::shared_ptr<AssetInfo>& info);
    void open_animation_editor_for_asset(const std::shared_ptr<AssetInfo>& info);
    void open_asset_info_editor_for_asset(Asset* asset, bool focus_camera = true);
    void close_asset_info_editor();
    bool consume_escape_for_asset_editor_stack();
    bool is_asset_info_editor_open() const;
    bool has_active_modal() const;
    void pulse_active_modal_header();

    void finalize_asset_drag(Asset* asset, const std::shared_ptr<AssetInfo>& info);

    void toggle_room_config();
    void open_room_config();
    void close_room_config();
    bool is_room_config_open() const;
    bool is_camera_settings_open() const;
    void regenerate_room();
    void regenerate_room_from_template(Room* source_room);

    using RoomAssetsSavedCallback = std::function<void()>;
    void set_room_assets_saved_callback(RoomAssetsSavedCallback cb);

    void focus_camera_on_asset(Asset* asset, double height_factor = 0.8, int duration_steps = 0);
    void focus_camera_on_room_center(bool reframe_height = true);

    void reset_click_state();
    void clear_selection();
    void clear_highlighted_assets();
    void purge_asset(Asset* asset);
    void set_pointer_queries_suspended(bool suspended);

    const std::vector<Asset*>& get_selected_assets() const { return selected_assets_; }
    const std::vector<Asset*>& get_highlighted_assets() const { return highlighted_assets_; }
    Asset* get_hovered_asset() const { return hovered_asset_; }

    void set_height_scale_factor(double factor);
    double get_height_scale_factor() const { return height_scale_factor_; }

    bool is_spawn_group_panel_visible() const;

protected:
    void handle_spawn_config_change(const nlohmann::json& entry);

private:
    enum class AssetEditorSubview;

    void begin_area_drag_session(const std::string& area_name, const SDL_Point& world_mouse);
    void update_area_drag_session(const SDL_Point& world_mouse);
    void finalize_area_drag_session();
    enum class GeometryHandle {
        None,
        Min,
        Max,
    };
    Room* find_geometry_room_at_point(SDL_Point world_point) const;
    bool geometry_room_is_trail(const Room* room) const;
    GeometryHandle hit_test_geometry_handle(Room* room, SDL_Point world_point) const;
    bool is_point_between_geometry_bounds(Room* room, SDL_Point world_point) const;
    void clear_geometry_selection();
    void mark_geometry_dirty(Room* room);
    bool regenerate_geometry(Room* room);
    nlohmann::json* find_area_entry_json(Room* room, const std::string& area_name) const;
    void ensure_area_anchor_spawn_entry(Room* room, const std::string& area_name);
    enum class BlockingPanel {
        Camera,
        MapLayers,
        AssetLibrary,
        Count,
};

    void set_blocking_panel_visible(BlockingPanel panel, bool visible);
    bool any_blocking_panel_visible() const;
    void open_room_config_for(Asset* asset);
    enum class DragMode {
        None,
        Free,
        Exact,
        Percent,
        Perimeter,
        PerimeterCenter,
        Edge,
};

    enum class ActiveModal {
        None,
        AssetInfo,
};

    struct PerimeterOverlay {
        SDL_Point center{0, 0};
        double radius = 0.0;
};

    struct DraggedAssetState {
        Asset*     asset     = nullptr;
        SDL_Point  start_pos {0, 0};
        SDL_Point  last_synced_pos {0, 0};
        SDL_FPoint direction {0.0f, 0.0f};
        bool       active    = false;
        double     edge_length = 0.0;
};
    struct CameraSettingsDragState {
        enum class Mode {
            None,
            Tilt,
            Pan,
        };
        Mode mode = Mode::None;
        Input::Button button = Input::LEFT;
        bool active = false;
};
    struct MousePressState {
        bool prev_left_down = false;
        SDL_Point press_screen{0, 0};
        Asset* pressed_asset = nullptr;
        const void* pressed_identity = nullptr;
        std::string pressed_spawn_id{};
        bool pressed_has_spawn_group = false;
        bool was_dragged = false;
        bool valid = false;
    };
    struct PendingSpawnGroupWork {
        std::string spawn_id{};
        nlohmann::json entry_snapshot = nlohmann::json::object();
        bool has_entry_snapshot = false;
        bool needs_respawn = false;
        bool needs_map_notify = false;
        bool needs_panel_refresh = false;
        bool needs_room_config_reopen = false;
        bool needs_selection_resync = false;
    };
    void handle_mouse_input(const Input& input);
    static DragMode drag_mode_for_spawn_method(const std::string& method, bool ctrl_modifier);
    bool is_asset_active_for_input(const Asset* asset) const;
    bool is_mouse_press_asset_valid() const;
    void clear_mouse_press_state(bool reset_click_tracking = false);
    bool consume_pressed_asset_release(Uint32 click_time_ms, std::string& out_spawn_id);
    bool apply_shift_edge_pan(const Input& input, WarpedScreenGrid& cam);
    static float edge_pan_intensity(int value, int max_value, float threshold_fraction);
    bool handle_camera_settings_mouse_controls(const Input& input);
    bool apply_scroll_size_adjustment(const Input& input);
    void apply_asset_scale_live_update(Asset* asset, int scale_percent);
    bool select_asset_or_group(Asset* asset);
    Asset* selected_asset_within_interaction_radius(SDL_Point screen_point) const;
    bool should_open_spawn_group_panel_for_click(const void* asset_identity,
                                                 bool has_spawn_group,
                                                 Uint32 click_time_ms);
    bool delete_selected_asset_or_group();
    Asset* hit_test_asset(SDL_Point screen_point, SDL_Renderer* renderer) const;
    bool asset_anchor_screen_position(const WarpedScreenGrid& cam, const Asset* asset, SDL_Point& out_screen) const;
    Asset* hit_test_asset_anchor(SDL_Point screen_point, int pick_radius_px) const;
    void update_hover_state(Asset* hit);
    void handle_click(const Input& input);
    std::optional<std::string> find_room_area_at_point(SDL_Point world_point);
    void update_highlighted_assets();
    bool is_ui_blocking_input(int mx, int my) const;
    bool should_enable_mouse_controls() const;
    void handle_shortcuts(const Input& input);
    void handle_delete_shortcut(const Input& input);
    void set_focus_asset(Asset* asset, bool from_asset_info);
    void set_focus_spawn_group(const std::string& spawn_id);
    void clear_focus();
    void validate_focus_state();
    void apply_focus_filter();
    bool focus_selection_matches_snapshot() const;
    void ensure_room_configurator();
    void ensure_spawn_group_config_ui();
    void update_room_config_bounds();
    void begin_drag_session(const SDL_Point& world_mouse, bool ctrl_modifier);
    void update_drag_session(const SDL_Point& world_mouse);
    void apply_perimeter_drag(const SDL_Point& world_mouse);
    void apply_edge_drag(const SDL_Point& world_mouse);
    bool snap_dragged_assets_to_grid();
    void sync_dragged_assets_immediately();
    void update_spawn_json_during_drag();
    void finalize_drag_session();
    void reset_drag_state();
    nlohmann::json* find_spawn_entry(const std::string& spawn_id);
    struct SpawnEntryResolution {
        enum class Source {
            None,
            Room,
            Map,
};
        nlohmann::json* entry = nullptr;
        nlohmann::json* owner_array = nullptr;
        Source source = Source::None;
        bool valid() const { return entry != nullptr; }
};
    SpawnEntryResolution locate_spawn_entry(const std::string& spawn_id);
    SDL_Point get_room_center() const;
    std::pair<int, int> get_room_dimensions() const;
    int current_grid_resolution() const;
    void refresh_spawn_group_config_ui();
    void update_spawn_group_config_anchor();
    SDL_Point spawn_groups_anchor_point() const;
    void clear_active_spawn_group_target();
    void sync_spawn_group_panel_with_selection();
    void update_grid_resolution_for_selection(Asset* primary);
    void clear_selection_grid_resolution_override();
    void update_exact_json(nlohmann::json& entry, const Asset& asset, SDL_Point center, int width, int height);
    void update_percent_json(nlohmann::json& entry, const Asset& asset, SDL_Point center, int width, int height);
    void save_perimeter_json(nlohmann::json& entry, int dx, int dy, int orig_w, int orig_h, int radius);
    void save_edge_json(nlohmann::json& entry, int inset_percent);
    const Area* find_edge_area_for_entry(const nlohmann::json& entry) const;
    double edge_length_along_direction(const Area& area, SDL_Point center, SDL_FPoint direction) const;
    void respawn_spawn_group(const nlohmann::json& entry);
    void prune_spawn_group_transient_references(const std::string& spawn_id);
    void enqueue_spawn_group_work(const nlohmann::json& entry,
                                  bool needs_respawn,
                                  bool needs_map_notify,
                                  bool needs_panel_refresh,
                                  bool needs_room_config_reopen,
                                  bool needs_selection_resync);
    void enqueue_spawn_group_ui_refresh(bool reopen_room_config, bool sync_selection);
    void process_pending_spawn_group_work();
    std::unique_ptr<vibble::grid::Occupancy> build_room_grid(const std::string& ignore_spawn_id) const;
    bool snap_spawn_group_to_resolution(Asset* anchor, int resolution);
    void render_room_trail_nav_buttons(SDL_Renderer* renderer);
    bool render_room_label(SDL_Renderer* renderer,
                           Room* room,
                           SDL_FPoint desired_center,
                           const SDL_Point& hover_point,
                           bool hover_enabled,
                           SDL_Rect* out_rect = nullptr);
    void clear_room_trail_nav_entries();
    bool handle_room_nav_click(const SDL_Point& screen_pt);
    void pan_camera_to_room(Room* room);
    SDL_Rect label_background_rect(int text_w, int text_h, SDL_FPoint desired_center) const;
    SDL_Rect resolve_edge_overlap(SDL_Rect rect, SDL_FPoint desired_center);
    SDL_Rect resolve_horizontal_edge_overlap(SDL_Rect rect, float desired_center_x, bool top_edge);
    SDL_Rect resolve_vertical_edge_overlap(SDL_Rect rect, float desired_center_y, bool left_edge);
    static bool rects_overlap(const SDL_Rect& a, const SDL_Rect& b);
    SDL_Rect effective_label_bounds() const;
    void ensure_label_font();
    void release_label_font();
    void invalidate_label_cache(Room* room);
    void invalidate_all_room_labels();
    void prune_label_cache(const std::vector<Room*>& rooms);
    void integrate_spawned_assets(std::vector<std::unique_ptr<Asset>>& spawned);
    void regenerate_current_room();
    void configure_shared_panel();
    void refresh_room_config_visibility();
    void sanitize_perimeter_spawn_groups();
    bool sanitize_perimeter_spawn_groups(nlohmann::json& groups);
    std::optional<PerimeterOverlay> compute_perimeter_overlay_for_drag();
    std::optional<PerimeterOverlay> compute_perimeter_overlay_for_spawn(const std::string& spawn_id);

    std::optional<std::vector<SDL_Point>> compute_edge_path_for_drag();
    std::optional<std::vector<SDL_Point>> compute_edge_path_for_spawn(const std::string& spawn_id);
    void add_spawn_group_internal();
    bool delete_spawn_group_internal(const std::string& spawn_id);
    bool remove_spawn_group_by_id(const std::string& spawn_id);
    void move_spawn_group_internal(const std::string& spawn_id, int dir);
    void reorder_spawn_group_internal(const std::string& spawn_id, size_t target_index);
    void open_spawn_group_editor_by_id(const std::string& spawn_id);
    void open_spawn_group_floating_panel(const std::string& spawn_id, std::optional<SDL_Point> screen_anchor = std::nullopt);
    void reopen_room_configurator();
    void notify_room_assets_saved();
    bool enqueue_current_room_save(devmode::core::DevSaveCoordinator::Priority priority);
    bool save_current_room_assets_json(devmode::core::DevSaveCoordinator::Priority priority =
                                           devmode::core::DevSaveCoordinator::Priority::Immediate);
    bool validate_room_edit_invariants(std::string* error = nullptr);
    bool commit_room_edit_transaction(const std::function<bool()>& mutate,
                                      const std::string& action_label,
                                      bool refresh_ui_on_success = true,
                                      devmode::core::DevSaveCoordinator::Priority save_priority =
                                          devmode::core::DevSaveCoordinator::Priority::Immediate);
    void mark_map_dirty_for_spawn_groups(devmode::core::DevSaveCoordinator::Priority priority =
                                             devmode::core::DevSaveCoordinator::Priority::Debounced);
    void copy_selected_spawn_group();
    void paste_spawn_group_from_clipboard();
    std::optional<std::string> selected_spawn_group_id() const;
    bool spawn_group_is_boundary(const std::string& spawn_id) const;
    Room* resolve_room_for_clipboard_action() const;
    void select_spawn_group_assets(const std::string& spawn_id);
    void remap_clipboard_entry_to_room(nlohmann::json& entry, Room* room);
    void ensure_clipboard_position_is_valid(nlohmann::json& entry, Room* room);
    static std::string strip_copy_suffix(const std::string& name);
    std::string next_clipboard_display_name();
    void show_notice(const std::string& message) const;
    static bool asset_info_contains_spawn_group(const class AssetInfo* info, const std::string& spawn_id);
    void mark_highlight_dirty();
    bool spawn_group_locked(const std::string& spawn_id) const;
    devmode::room_selection_filter::SelectionFilter effective_selection_filter() const;
    devmode::room_selection_filter::SpawnOwnership classify_spawn_group_ownership(const std::string& spawn_id) const;
    devmode::room_selection_filter::SpawnOwnership classify_asset_ownership(const Asset* asset) const;
    bool owner_array_matches_map_section(const nlohmann::json* owner_array, const char* section_key) const;
    bool asset_matches_selection_filter(const Asset* asset) const;
    struct DynamicBoundaryProxyKey {
        std::string spawn_id;
        std::string asset_name;
        int boundary_type_index = -1;
        int candidate_index = -1;
        int world_x = 0;
        int world_z = 0;

        bool valid() const { return !spawn_id.empty(); }
        bool operator==(const DynamicBoundaryProxyKey& other) const {
            return spawn_id == other.spawn_id &&
                   asset_name == other.asset_name &&
                   boundary_type_index == other.boundary_type_index &&
                   candidate_index == other.candidate_index &&
                   world_x == other.world_x &&
                   world_z == other.world_z;
        }
        bool operator!=(const DynamicBoundaryProxyKey& other) const {
            return !(*this == other);
        }
    };
    struct DynamicBoundaryProxyHit {
        DynamicBoundaryProxyKey key{};
        SDL_FRect screen_rect{0.0f, 0.0f, 0.0f, 0.0f};
        int world_z = 0;
    };
    std::optional<DynamicBoundaryProxyHit> hit_test_dynamic_boundary_sprite(SDL_Point screen_point) const;
    std::optional<SDL_FRect> dynamic_boundary_proxy_rect(const DynamicBoundaryProxyKey& key) const;
    void clear_dynamic_boundary_proxy_selection();
    bool open_asset_info_for_dynamic_boundary(const DynamicBoundaryProxyHit& hit);
    void render_dynamic_boundary_proxy_overlay(SDL_Renderer* renderer) const;
    void cycle_selection_filter();
    void reset_selection_filter();
    void ensure_anchor_editor_widgets();
    void ensure_oval_editor_widgets();
    void ensure_movement_editor_widgets();
    void ensure_hitbox_editor_widgets();
    void ensure_attack_box_editor_widgets();
    void ensure_attack_payload_editor_widget();
    void update_asset_editor_layout();
    bool should_show_asset_editor_navigation() const;
    bool anchor_mode_active() const;
    bool light_mode_active() const;
    bool oval_mode_active() const;
    bool movement_mode_active() const;
    bool hitbox_mode_active() const;
    bool attack_box_mode_active() const;
    bool is_asset_pointer_live(const Asset* asset) const;
    Asset* selected_anchor_mode_asset() const;
    AssetEditorSubview next_asset_editor_subview(AssetEditorSubview subview) const;
    bool can_enter_asset_editor_subview(AssetEditorSubview subview) const;
    void cycle_asset_editor_subview();
    void set_asset_editor_subview(AssetEditorSubview subview, bool animate);
    void apply_asset_editor_subview_change(AssetEditorSubview subview, bool animate);
    void drain_pending_asset_editor_subview_request();
    void process_pending_animation_editor_close();
    void on_animation_editor_closed();
    void begin_asset_editor_transition(AssetEditorSubview from, AssetEditorSubview to);
    void update_asset_editor_transition();
    void apply_asset_editor_panel_overrides();
    bool asset_editor_tab_scope_active() const;
    void toggle_anchor_edit_mode();
    bool enter_anchor_edit_mode(bool light_editor_mode = false);
    void exit_anchor_edit_mode(bool flush_immediately);
    bool enter_oval_anchor_edit_mode();
    void exit_oval_anchor_edit_mode(bool flush_immediately);
    bool enter_movement_edit_mode();
    void exit_movement_edit_mode(bool persist_changes);
    bool enter_hitbox_edit_mode();
    void exit_hitbox_edit_mode(bool persist_changes);
    bool enter_attack_box_edit_mode();
    void exit_attack_box_edit_mode(bool persist_changes);
    void validate_anchor_edit_target();
    void validate_oval_edit_target();
    void validate_movement_edit_target();
    void validate_hitbox_edit_target();
    void validate_attack_box_edit_target();
    bool is_anchor_ui_blocking_point(int x, int y) const;
    bool is_oval_ui_blocking_point(int x, int y) const;
    bool is_movement_ui_blocking_point(int x, int y) const;
    bool is_hitbox_ui_blocking_point(int x, int y) const;
    bool is_attack_box_ui_blocking_point(int x, int y) const;
    bool any_editor_point_selected() const;
    enum class EditorFramePropagationScope {
        NextFrame,
        Animation,
        Asset,
    };
    void navigate_anchor_animation(int delta);
    void navigate_anchor_frame(int delta);
    void navigate_oval_animation(int delta);
    void navigate_oval_frame(int delta);
    void navigate_movement_animation(int delta);
    void navigate_movement_frame(int delta);
    void navigate_hitbox_animation(int delta);
    void navigate_hitbox_frame(int delta);
    void navigate_attack_box_animation(int delta);
    void navigate_attack_box_frame(int delta);
    void navigate_asset_info_preview_animation(int delta);
    void navigate_asset_info_preview_frame(int delta);
    bool apply_anchor_animation_and_frame(const std::string& animation_id, int frame_index);
    bool apply_oval_animation_and_frame(const std::string& animation_id, int frame_index);
    bool apply_movement_animation_and_frame(const std::string& animation_id, int frame_index);
    bool apply_hitbox_animation_and_frame(const std::string& animation_id, int frame_index);
    bool apply_attack_box_animation_and_frame(const std::string& animation_id, int frame_index);
    bool apply_asset_preview_animation_and_frame(Asset* target, const std::string& animation_id, int frame_index);
    std::vector<std::string> anchor_mode_animation_names() const;
    std::vector<std::string> oval_mode_animation_names() const;
    std::vector<std::string> movement_mode_animation_names() const;
    std::vector<std::string> hitbox_mode_animation_names() const;
    std::vector<std::string> attack_box_mode_animation_names() const;
    int resolve_anchor_mode_frame_index() const;
    int resolve_oval_mode_frame_index() const;
    int resolve_movement_mode_frame_index() const;
    int resolve_hitbox_mode_frame_index() const;
    int resolve_attack_box_mode_frame_index() const;
    void refresh_anchor_mode_handles();
    void sync_anchor_tools_panel();
    void sync_oval_tools_panel();
    bool selected_oval_mapping_binding_valid() const;
    bool resolve_selected_oval_lock_target(float& out_world_x, float& out_world_z, float& out_heading_radians) const;
    void sync_oval_attachment_lock();
    void release_oval_attachment_lock();
    bool ensure_selected_anchor_light_attachment();
    void sync_anchor_candidate_editor();
    void refresh_anchor_candidate_editor_widget();
    void update_anchor_candidate_editor_search(const Input& input);
    void layout_anchor_candidate_editor_popup();
    void open_anchor_candidate_editor(const std::string& anchor_name, SDL_Point click_point, const SDL_Rect& row_rect);
    void close_anchor_candidate_editor();
    Asset* active_anchor_candidate_target_asset() const;
    bool anchor_candidate_editor_mode_active() const;
    bool anchor_candidate_anchor_exists_for_target(const Asset* target, const std::string& anchor_name) const;
    bool handle_anchor_candidate_editor_event(const SDL_Event& event);
    void render_anchor_candidate_editor(SDL_Renderer* renderer) const;
    bool mutate_anchor_candidate_entry(const std::function<bool(nlohmann::json&)>& mutator,
                                       devmode::core::DevSaveCoordinator::Priority priority,
                                       bool flush_now,
                                       const char* reason,
                                       const char* flush_tag);
    std::vector<std::string> canonical_anchor_names_for_eligible_animations(const AssetInfo& info) const;
    bool reconcile_anchor_child_candidates_with_eligible_names(const std::shared_ptr<AssetInfo>& target_info,
                                                               bool& changed);
    void sync_hitbox_tools_panel();
    void sync_attack_box_tools_panel();
    void sync_attack_payload_editor();
    void ensure_anchor_selection_valid();
    bool anchor_visible_in_current_mode(const DisplacedAssetAnchorPoint& anchor) const;
    bool anchor_mutable_in_current_mode(const DisplacedAssetAnchorPoint& anchor) const;
    bool anchor_name_exists_across_eligible_animations(const std::shared_ptr<AssetInfo>& target_info,
                                                       const std::string& name) const;
    void rebuild_movement_rel_positions();
    void rebuild_movement_frames_from_positions();
    void normalize_movement_frames_to_current_animation();
    void refresh_movement_runtime_animation();
    bool persist_movement_current_animation(devmode::core::DevSaveCoordinator::Priority priority);
    void refresh_movement_editor_selection(bool reset_drag_state);
    int find_movement_point_at_screen_point(SDL_Point screen_point, int radius_px) const;
    bool project_movement_point(std::size_t index, SDL_FPoint& out_screen) const;
    bool handle_movement_mode_mouse_input(const Input& input);
    void apply_movement_linear_smoothing(int adjusted_index,
                                         std::vector<SDL_FPoint>& redistributed_xy,
                                         std::vector<float>& redistributed_z,
                                         int last_index) const;
    void apply_movement_curved_smoothing(int adjusted_index,
                                         const std::vector<SDL_FPoint>& original_xy,
                                         const std::vector<float>& original_z,
                                         std::vector<SDL_FPoint>& redistributed_xy,
                                         std::vector<float>& redistributed_z,
                                         int last_index) const;
    void redistribute_movement_points_after_adjustment(int adjusted_index);
    SDL_Point movement_asset_anchor_world() const;
    float movement_base_world_z() const;
    devmode::FileSourcedAnimationSelection resolve_file_sourced_animation_selection_for_target(const Asset* target,
                                                                                              const std::string& animation_id) const;
    int find_anchor_handle_at_point(SDL_Point screen_point, int radius_px, const std::string& preferred_anchor = {}) const;
    bool handle_anchor_mode_mouse_input(const Input& input);
    int find_oval_point_handle_at_point(SDL_Point screen_point, int radius_px, int preferred_point_index = -1) const;
    bool handle_oval_mode_mouse_input(const Input& input);
    bool mutate_anchor_current_frame(const std::function<bool(std::vector<DisplacedAssetAnchorPoint>&)>& mutator,
                                     devmode::core::DevSaveCoordinator::Priority priority);
    bool persist_anchor_current_frame(devmode::core::DevSaveCoordinator::Priority priority, bool flush_now);
    bool apply_anchor_panel_detail_update(const RoomAnchorToolsPanel::DetailValues& values);
    bool apply_anchor_panel_light_update(const RoomAnchorToolsPanel::LightValues& values);
    bool update_anchor_depth(const std::string& anchor_name, float delta_world);
    bool drag_anchor_to_screen(const std::string& anchor_name, SDL_Point screen_point);
    bool add_anchor_in_current_frame();
    bool rename_selected_anchor_in_current_frame(const std::string& desired_name);
    bool delete_selected_anchor_in_current_frame();
    bool add_oval_mapping();
    bool delete_selected_oval_mapping();
    bool apply_selected_oval_properties(const RoomOvalToolsPanel::OvalProperties& properties);
    bool increment_selected_oval_point_count();
    bool decrement_selected_oval_point_count();
    bool apply_selected_oval_point_details(const RoomOvalToolsPanel::PointDetailValues& values);
    bool apply_selected_oval_center_details(const RoomOvalToolsPanel::CenterDetailValues& values);
    bool apply_oval_center_current_frame_to_scope(EditorFramePropagationScope scope);
    bool drag_oval_center_to_screen(SDL_Point screen_point);
    std::string selected_oval_center_anchor_name() const;
    bool mutate_selected_oval_center_anchor(
        const std::function<bool(DisplacedAssetAnchorPoint&)>& mutator,
        devmode::core::DevSaveCoordinator::Priority priority);
    bool persist_oval_mappings(devmode::core::DevSaveCoordinator::Priority priority,
                               bool flush_now,
                               const char* reason,
                               const char* flush_tag);
    void refresh_oval_mode_handles();
    std::unordered_set<std::string> valid_oval_center_anchor_names(const AssetInfo& info) const;
    bool is_valid_oval_center_anchor_name(const std::string& anchor_name) const;
    bool selected_anchor_is_oval_center() const;
    bool apply_anchor_current_frame_to_scope(EditorFramePropagationScope scope);
    bool normalize_anchor_invariants_for_eligible_animations(Asset* target,
                                                              const std::shared_ptr<AssetInfo>& target_info,
                                                              bool& updated_any);
    bool commit_anchor_bulk_edit(Asset* target,
                                 const std::shared_ptr<AssetInfo>& target_info,
                                 devmode::core::DevSaveCoordinator::Priority priority,
                                 bool flush_now,
                                 const char* reason,
                                 const char* flush_tag);
    std::vector<std::string> eligible_anchor_animation_names(const AssetInfo& info) const;
    int find_hitbox_corner_at_screen_point(SDL_Point screen_point,
                                           int radius_px,
                                           int& out_corner_index,
                                           int& out_point_index) const;
    int find_attack_box_corner_at_screen_point(SDL_Point screen_point,
                                               int radius_px,
                                               int& out_corner_index,
                                               int& out_point_index) const;
    int find_hitbox_rotation_handle_at_screen_point(SDL_Point screen_point) const;
    int find_attack_box_rotation_handle_at_screen_point(SDL_Point screen_point) const;
    int find_hitbox_body_at_screen_point(SDL_Point screen_point) const;
    int find_attack_box_body_at_screen_point(SDL_Point screen_point) const;
    bool handle_hitbox_mode_mouse_input(const Input& input);
    bool handle_attack_box_mode_mouse_input(const Input& input);
    bool mutate_hitbox_current_frame(const std::function<bool(std::vector<animation_update::FrameHitBox>&)>& mutator,
                                     devmode::core::DevSaveCoordinator::Priority priority);
    bool mutate_attack_box_current_frame(const std::function<bool(std::vector<animation_update::FrameAttackBox>&)>& mutator,
                                         devmode::core::DevSaveCoordinator::Priority priority);
    bool persist_hitbox_current_frame(devmode::core::DevSaveCoordinator::Priority priority, bool flush_now);
    bool persist_attack_box_current_frame(devmode::core::DevSaveCoordinator::Priority priority, bool flush_now);
    bool persist_specific_attack_box_frame(int frame_index, devmode::core::DevSaveCoordinator::Priority priority);
    bool drag_hitbox_corner_to_screen(int box_index, int point_index, SDL_Point screen_point);
    bool drag_attack_box_corner_to_screen(int box_index, int point_index, SDL_Point screen_point);
    bool begin_hitbox_box_drag(int box_index, SDL_Point screen_point);
    bool begin_attack_box_drag(int box_index, SDL_Point screen_point);
    bool begin_hitbox_rotation_drag(int box_index, SDL_Point screen_point);
    bool begin_attack_box_rotation_drag(int box_index, SDL_Point screen_point);
    bool drag_hitbox_box_to_screen(int box_index, SDL_Point screen_point);
    bool drag_attack_box_to_screen(int box_index, SDL_Point screen_point);
    bool drag_hitbox_rotation_to_screen(int box_index, SDL_Point screen_point);
    bool drag_attack_box_rotation_to_screen(int box_index, SDL_Point screen_point);
    bool add_hitbox_in_current_frame();
    bool add_attack_box_in_current_frame();
    bool delete_selected_hitbox_in_current_frame();
    bool delete_selected_attack_box_in_current_frame();
    bool apply_hitbox_current_frame_to_scope(EditorFramePropagationScope scope);
    bool apply_attack_box_current_frame_to_scope(EditorFramePropagationScope scope);
    bool apply_hitbox_panel_detail_update(const RoomBoxToolsPanel::DetailValues& values);
    bool apply_attack_box_panel_detail_update(const RoomBoxToolsPanel::DetailValues& values);
    bool apply_attack_payload_editor_update(const animation_update::AttackPayload& payload);

    struct AssetSpatialEntry {
        SDL_Rect bounds{0, 0, 0, 0};
        int screen_y = std::numeric_limits<int>::min();
        std::vector<int64_t> cells;
};

    void render_asset_outline(SDL_Renderer* renderer, Asset* asset, const WarpedScreenGrid& cam, const SDL_Color& color, int outline_offset_px) const;

    void mark_spatial_index_dirty() const;
    const std::vector<Asset*>* selection_asset_source() const;
    bool ensure_spatial_index(const WarpedScreenGrid& cam) const;
    bool camera_state_changed(const WarpedScreenGrid& cam) const;
      bool compute_asset_screen_bounds(const WarpedScreenGrid& cam, Asset* asset, SDL_Rect& out_rect, int& out_screen_y) const;
    bool compute_asset_render_object_bounds(const WarpedScreenGrid& cam, Asset* asset, SDL_Rect& out_rect) const;
    void rebuild_spatial_index(const WarpedScreenGrid& cam) const;
    void insert_asset_entry(Asset* asset, const SDL_Rect& rect, int screen_y) const;
    void add_asset_to_cell(Asset* asset, int cell_x, int cell_y, std::vector<int64_t>& cell_keys) const;
    void remove_asset_from_spatial_index(Asset* asset) const;
    void refresh_asset_spatial_entry(const WarpedScreenGrid& cam, Asset* asset) const;
    void refresh_spatial_entries_for_dragged_assets();
    std::vector<Asset*> gather_candidate_assets_for_point(SDL_Point screen_point) const;
    Asset* hit_test_asset_fallback(const WarpedScreenGrid& cam, SDL_Point screen_point) const;

private:
    Assets* assets_ = nullptr;
    Input* input_ = nullptr;
    std::vector<Asset*>* active_assets_ = nullptr;
    std::uint64_t active_assets_version_ = 0;
    Asset* player_ = nullptr;
    Room* current_room_ = nullptr;
    bool room_locked_for_edit_ = false;

    int screen_w_ = 0;
    int screen_h_ = 0;
    bool enabled_ = false;
    bool mouse_controls_enabled_last_frame_ = false;

    enum class SelectionFilter {
        All,             // All selectable assets
        Normal,          // Primary room assets (excluding boundary-domain, tiled, and anchored)
        Tiled,           // Tiled assets only
        Boundary,        // map_boundary_data spawn-group assets/sprites only
        Anchored,        // Assets following another asset's anchor point
    };

    enum class EditorMode {
        Normal,
        AnchorEdit,
        LightEdit,
        OvalAnchorEdit,
        MovementEdit,
        HitBoxEdit,
        AttackBoxEdit,
    };

    enum class AssetEditorSubview {
        AssetInfo,
        AnimationEditor,
        Anchor,
        Light,
        OvalAnchor,
        Movement,
        Hitbox,
        AttackBox,
    };

    SelectionFilter selection_filter_ = SelectionFilter::Normal;
    EditorMode editor_mode_ = EditorMode::Normal;
    bool shift_was_down_last_frame_ = false;
    bool shift_space_was_down_last_frame_ = false;

    std::unique_ptr<AssetLibraryUI> library_ui_;
    std::unique_ptr<AssetInfoUI> info_ui_;
    std::unique_ptr<RoomAnchorToolsPanel> anchor_tools_panel_;
    std::unique_ptr<RoomOvalToolsPanel> oval_tools_panel_;
    std::unique_ptr<RoomMovementToolsPanel> movement_tools_panel_;
    std::unique_ptr<RoomBoxToolsPanel> hitbox_tools_panel_;
    std::unique_ptr<RoomBoxToolsPanel> attack_box_tools_panel_;
    std::unique_ptr<devmode::room_config::AttackPayloadEditor> attack_payload_editor_;
    std::unique_ptr<BottomNavigationPanel> anchor_navigation_panel_;

    struct AnchorHandleSample {
        std::string name;
        int texture_x = 0;
        int texture_y = 0;
        float depth_offset = 0.0f;
        bool flip_horizontal = true;
        bool flip_vertical = true;
        float rotation_degrees = 0.0f;
        bool hidden = false;
        bool resolve_x = true;
        AnchorScalingMethod scaling_method = AnchorScalingMethod::Parent;
        bool has_light_data = false;
        AnchorLightData light{};
        SDL_FPoint flat_screen_px{0.0f, 0.0f};
        bool has_flat_screen_px = false;
        SDL_FPoint final_screen_px{0.0f, 0.0f};
        bool has_final_screen_px = false;
    };

    struct AnchorEditState {
        Asset* target_asset = nullptr;
        std::string animation_id;
        int frame_index = 0;
        std::string selected_anchor_name;
        std::string hovered_anchor_name;
        std::string dragging_anchor_name;
        bool point_selected = false;
        bool dragging = false;
        bool onion_skin_enabled = false;
        bool had_static_frame_before = false;
        bool static_frame_before = false;
        bool dirty_since_last_flush = false;
        bool light_editor_mode = false;
        std::vector<AnchorHandleSample> handles;
    };
    AnchorEditState anchor_edit_;

    struct OvalPointHandleSample {
        int point_index = -1;
        float angle_degrees = 0.0f;
        SDL_FPoint flat_screen_px{0.0f, 0.0f};
        bool has_flat_screen_px = false;
        SDL_FPoint final_screen_px{0.0f, 0.0f};
        bool has_final_screen_px = false;
    };

    struct OvalAnchorEditState {
        Asset* target_asset = nullptr;
        std::string animation_id;
        int frame_index = 0;
        int selected_oval_index = -1;
        int selected_point_index = -1;
        int hovered_point_index = -1;
        bool center_selected = false;
        bool center_hovered = false;
        bool center_dragging = false;
        bool attachment_lock_active = false;
        bool attachment_lock_had_heading = false;
        float attachment_lock_heading_radians = 0.0f;
        bool attachment_lock_had_target = false;
        float attachment_lock_target_world_x = 0.0f;
        float attachment_lock_target_world_z = 0.0f;
        bool had_static_frame_before = false;
        bool static_frame_before = false;
        bool dirty_since_last_flush = false;
        SDL_FPoint center_screen_px{0.0f, 0.0f};
        bool has_center_screen_px = false;
        float center_world_x = 0.0f;
        float center_world_y = 0.0f;
        float center_world_z = 0.0f;
        bool has_center_world = false;
        std::vector<SDL_FPoint> guide_screen_samples;
        std::vector<OvalPointHandleSample> handles;
    };
    OvalAnchorEditState oval_edit_;

    struct AnchorCandidateEditorState {
        bool open = false;
        std::string anchor_name;
        Asset* target_asset = nullptr;
        SDL_Point open_point{0, 0};
        SDL_Rect anchor_row_rect{0, 0, 0, 0};
        std::unique_ptr<DockableCollapsible> panel{};
        std::unique_ptr<CandidateEditorPieGraphWidget> pie_widget{};
    };
    AnchorCandidateEditorState anchor_candidate_editor_;

    struct MovementEditState {
        Asset* target_asset = nullptr;
        std::string animation_id;
        int frame_index = 0;
        bool point_selected = false;
        bool selected_point_active = false;
        int hovered_point_index = -1;
        bool dragging_point = false;
        bool had_static_frame_before = false;
        bool static_frame_before = false;
        bool dirty_since_last_flush = false;
        bool smooth_enabled = false;
        bool curve_enabled = false;
        std::vector<devmode::room_movement_payload::MovementFrame> frames;
        std::vector<SDL_FPoint> rel_positions;
        std::vector<float> rel_positions_z;

        bool has_frames() const { return !frames.empty(); }
        std::size_t frame_count() const { return frames.size(); }
    };
    MovementEditState movement_edit_;

    struct BoxEditState {
        Asset* target_asset = nullptr;
        std::string animation_id;
        int frame_index = 0;
        int selected_box_index = -1;
        int selected_corner_index = 0;
        int selected_point_index = 0;
        int hovered_box_index = -1;
        int hovered_corner_index = -1;
        int hovered_point_index = -1;
        bool point_selected = false;
        bool dragging_corner = false;
        bool dragging_box = false;
        bool dragging_rotation = false;
        bool hovered_rotation_handle = false;
        bool onion_skin_enabled = false;
        int drag_reference_point_index = -1;
        int drag_reference_corner_index = -1;
        SDL_FPoint drag_reference_screen_offset{0.0f, 0.0f};
        animation_update::FrameBoxRect drag_start_rect{};
        SDL_FPoint rotation_drag_center_screen{0.0f, 0.0f};
        float rotation_drag_start_angle_degrees = 0.0f;
        float rotation_drag_start_box_rotation_degrees = 0.0f;
        bool had_static_frame_before = false;
        bool static_frame_before = false;
        bool dirty_since_last_flush = false;
    };
    BoxEditState hitbox_edit_;
    BoxEditState attack_box_edit_;

    struct AssetEditorTransitionState {
        bool active = false;
        AssetEditorSubview from = AssetEditorSubview::AssetInfo;
        AssetEditorSubview to = AssetEditorSubview::AssetInfo;
        int frame = 0;
        int duration_frames = 12;
    };
    struct PendingAssetEditorSubviewRequest {
        AssetEditorSubview subview = AssetEditorSubview::AssetInfo;
        bool animate = true;
    };
    AssetEditorSubview asset_editor_subview_ = AssetEditorSubview::AssetInfo;
    AssetEditorSubview previous_non_animation_subview_ = AssetEditorSubview::AssetInfo;
    AssetEditorTransitionState asset_editor_transition_{};
    bool asset_editor_subview_change_in_progress_ = false;
    std::optional<PendingAssetEditorSubviewRequest> pending_asset_editor_subview_request_{};
    std::optional<AssetEditorSubview> pending_animation_editor_close_subview_{};

    std::unique_ptr<RoomConfigurator> room_cfg_ui_;
    SDL_Rect room_config_bounds_{0, 0, 0, 0};
    DevFooterBar* shared_footer_bar_ = nullptr;
    bool room_config_dock_open_ = false;
    bool room_config_was_visible_ = false;
    bool suppress_room_config_selection_clear_ = false;
    ActiveModal active_modal_ = ActiveModal::None;
    std::function<void(bool)> header_visibility_callback_{};
    std::function<void()> open_boundary_assets_panel_callback_{};
    bool room_config_panel_visible_ = false;
    bool asset_info_panel_visible_ = false;

    std::array<bool, static_cast<size_t>(BlockingPanel::Count)> blocking_panel_visible_{};

    Asset* hovered_asset_ = nullptr;
    Asset* hovered_anchor_asset_ = nullptr;
    bool pointer_queries_suspended_ = false;
    std::vector<Asset*> selected_assets_;
    std::vector<Asset*> highlighted_assets_;
    std::vector<Asset*> focus_selection_snapshot_;
    std::optional<std::string> focus_spawn_group_snapshot_{};
    Asset* focused_asset_ = nullptr;
    std::optional<std::string> focused_spawn_id_{};
    bool focus_active_ = false;
    bool focus_from_asset_info_ = false;
    bool highlight_dirty_ = true;

    bool snap_to_grid_enabled_ = true;
    SDL_Point snapped_cursor_world_{0, 0};
    int cursor_snap_resolution_ = 0;
    SDL_Point last_raw_mouse_world_{0, 0};
    bool has_last_raw_mouse_world_ = false;

    bool dragging_ = false;
    Asset* drag_anchor_asset_ = nullptr;
    DragMode drag_mode_ = DragMode::None;
    std::vector<DraggedAssetState> drag_states_;
    SDL_Point drag_last_world_{0, 0};
    SDL_Point drag_room_center_{0, 0};
    SDL_Point drag_perimeter_circle_center_{0, 0};
    double drag_perimeter_base_radius_ = 0.0;
    SDL_Point drag_perimeter_center_offset_world_{0, 0};
    int drag_perimeter_orig_w_ = 0;
    int drag_perimeter_orig_h_ = 0;
    int drag_perimeter_curr_w_ = 0;
    int drag_resolution_ = 0;

    const Area* drag_edge_area_ = nullptr;
    SDL_Point drag_edge_center_{0, 0};
    double drag_edge_inset_percent_ = 100.0;

    devmode::core::ManifestStore* manifest_store_ = nullptr;
    devmode::core::DevSaveCoordinator* save_coordinator_ = nullptr;
    devmode::core::SaveManager* save_manager_ = nullptr;
    std::function<void(devmode::core::DevSaveCoordinator::Priority)> mark_map_dirty_callback_;
    int drag_perimeter_curr_h_ = 0;
    bool drag_moved_ = false;
    std::string drag_spawn_id_;
    bool suppress_next_left_click_ = false;

    std::optional<int> selection_overlay_resolution_before_override_{};
    std::optional<int> selection_overlay_resolution_override_{};

    int click_buffer_frames_ = 0;
    int rclick_buffer_frames_ = 0;
    int hover_miss_frames_ = 0;
    const void* last_click_asset_ = nullptr;
    Uint32 last_click_time_ms_ = 0;
    MousePressState mouse_press_state_{};
    int suppress_world_left_click_frames_ = 0;
    bool spawn_group_panel_sync_in_progress_ = false;
    bool spawn_group_panel_open_in_progress_ = false;
    bool spawn_group_callback_in_progress_ = false;
    bool processing_pending_spawn_group_work_ = false;
    std::unordered_map<std::string, PendingSpawnGroupWork> pending_spawn_group_work_{};
    bool pending_spawn_group_panel_refresh_ = false;
    bool pending_spawn_group_room_config_reopen_ = false;
    bool pending_spawn_group_selection_resync_ = false;
    std::optional<SDL_Point> pending_spawn_world_pos_{};
    std::optional<std::string> active_spawn_group_id_{};
    std::uint64_t room_assets_edit_version_ = 0;
    bool suppress_spawn_group_close_clear_ = false;
    std::unique_ptr<SpawnGroupConfig> spawn_group_panel_{};

    bool area_dragging_ = false;
    bool area_drag_moved_ = false;
    std::string area_drag_name_;
    int area_drag_resolution_ = 0;
    SDL_Point area_drag_start_world_{0, 0};
    SDL_Point area_drag_last_world_{0, 0};
    Room* hovered_geometry_room_ = nullptr;
    Room* selected_geometry_room_ = nullptr;
    GeometryHandle geometry_drag_handle_ = GeometryHandle::None;
    bool geometry_drag_pending_dirty_ = false;
    Uint32 geometry_last_click_ms_ = 0;

    struct SpawnGroupClipboard {
        nlohmann::json entry;
        std::string base_display_name;
        int paste_count = 0;
};
    std::optional<SpawnGroupClipboard> spawn_group_clipboard_{};

    TTF_Font* label_font_ = nullptr;
    std::vector<SDL_Rect> label_rects_;
    struct LabelCacheEntry {
        SDL_Texture* texture = nullptr;
        SDL_Point text_size{0, 0};
        std::string last_name;
        SDL_Color last_color{0, 0, 0, 0};
        bool dirty = true;
    };
    std::unordered_map<Room*, LabelCacheEntry> label_cache_;
    SDL_Rect active_label_bounds_{0, 0, 0, 0};
    struct RoomNavEntry {
        Room*   room = nullptr;
        SDL_Rect rect{0, 0, 0, 0};
        bool    is_trail = false;
    };
    std::vector<RoomNavEntry> room_nav_entries_;
    bool room_nav_visible_ = false;

    double height_scale_factor_ = 1.1;
    DevCameraControls camera_controls_;
    bool camera_pan_active_notified_ = false;
    bool camera_pan_just_finished_ = false;
    int suppress_left_click_frames_ = 0;
    bool camera_settings_drag_active_notified_ = false;
    struct CameraLockState {
        bool valid = false;
        bool manual_height_override = false;
        bool manual_zoom_override = false;
        bool had_focus_override = false;
        SDL_Point focus_point{0, 0};
        SDL_Point screen_center{0, 0};
        double camera_zoom_percent = 0.0;
    };
    CameraLockState camera_lock_restore_{};
    bool camera_settings_lock_active_ = false;
    CameraSettingsDragState camera_settings_drag_{};
    std::unordered_set<std::string> room_spawn_ids_;
    std::unordered_set<std::string> map_boundary_spawn_ids_;
    void rebuild_room_spawn_id_cache();
    bool is_room_spawn_id(const std::string& spawn_id) const;
    bool asset_belongs_to_room(const Asset* asset) const;
    std::optional<DynamicBoundaryProxyKey> selected_dynamic_boundary_proxy_{};
    std::optional<DynamicBoundaryProxyKey> hovered_dynamic_boundary_proxy_{};

    RoomAssetsSavedCallback room_assets_saved_callback_;
    std::string rename_active_room(const std::string& old_name, const std::string& desired_name);
    std::shared_ptr<AssetInfo> last_selected_from_library_;

    friend class DevControls;
#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
    std::uint32_t test_snap_spawn_group_to_resolution_call_count_ = 0;
    std::uint32_t test_respawn_spawn_group_call_count_ = 0;
    friend struct RoomEditorTestAccess;
#endif

    static constexpr int kSpatialCellSize = 256;

    mutable bool spatial_index_dirty_ = true;
    mutable bool cached_camera_state_valid_ = false;
    mutable float cached_camera_scale_ = 0.0f;
    mutable SDL_Point cached_camera_center_{0, 0};
    mutable double cached_camera_zoom_percent_ = 0.0;
    mutable float cached_camera_pitch_deg_ = 0.0f;
    mutable double cached_camera_anchor_world_z_ = 0.0;
    mutable std::unordered_map<Asset*, AssetSpatialEntry> asset_bounds_cache_;
    mutable std::unordered_map<int64_t, std::vector<Asset*>> spatial_grid_;
};

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
struct RoomEditorTestAccess {
    static int subview_asset_info();
    static int subview_animation_editor();
    static int subview_anchor();

    static int active_subview(const RoomEditor& editor);
    static void set_active_subview(RoomEditor& editor, int subview);
    static void set_subview_change_in_progress(RoomEditor& editor, bool in_progress);

    static bool has_pending_subview_request(const RoomEditor& editor);
    static int pending_subview(const RoomEditor& editor);
    static bool pending_subview_animate(const RoomEditor& editor);
    static bool has_pending_animation_editor_close_subview(const RoomEditor& editor);
    static int pending_animation_editor_close_subview(const RoomEditor& editor);

    static void request_subview(RoomEditor& editor, int subview, bool animate);
    static void drain_pending_subview_request(RoomEditor& editor);
    static void process_pending_animation_editor_close(RoomEditor& editor);
    static void invoke_on_animation_editor_closed(RoomEditor& editor);
    static void set_snap_to_grid_enabled(RoomEditor& editor, bool enabled);
    static void set_shared_footer_present(RoomEditor& editor, bool present);
    static void update_grid_resolution_for_selection(RoomEditor& editor, const void* primary_asset_identity);
    static std::uint32_t snap_spawn_group_to_resolution_call_count(const RoomEditor& editor);
    static void reset_snap_spawn_group_to_resolution_call_count(RoomEditor& editor);
    static bool should_open_spawn_group_panel_for_click(RoomEditor& editor,
                                                        const void* asset_identity,
                                                        bool has_spawn_group,
                                                        std::uint32_t click_time_ms);
    static void reset_click_tracking(RoomEditor& editor);
    static bool consume_pressed_asset_release(RoomEditor& editor,
                                              std::uint32_t click_time_ms,
                                              std::string& out_spawn_id);
    static void set_active_asset_identities(RoomEditor& editor, const std::vector<const void*>& identities);
    static void set_mouse_press_state(RoomEditor& editor,
                                      const void* asset_identity,
                                      const std::string& spawn_id,
                                      bool has_spawn_group,
                                      bool was_dragged);
    static void clear_mouse_press_state(RoomEditor& editor, bool reset_click_tracking);
    static int drag_mode_for_spawn_method(const std::string& method, bool ctrl_modifier);
    static void set_spawn_group_callback_in_progress(RoomEditor& editor, bool in_progress);
    static bool spawn_group_callback_in_progress(const RoomEditor& editor);
    static void enqueue_spawn_group_work(RoomEditor& editor,
                                         const std::string& spawn_id,
                                         bool needs_respawn,
                                         bool needs_panel_refresh,
                                         bool needs_room_config_reopen,
                                         bool needs_selection_resync);
    static std::size_t pending_spawn_group_work_size(const RoomEditor& editor);
    static void process_pending_spawn_group_work(RoomEditor& editor);
    static std::uint32_t respawn_spawn_group_call_count(const RoomEditor& editor);
    static void reset_respawn_spawn_group_call_count(RoomEditor& editor);
};
#endif
