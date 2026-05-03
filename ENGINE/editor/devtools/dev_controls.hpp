#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <filesystem>

#include <nlohmann/json_fwd.hpp>

#include "core/AssetsManager.hpp"
#include "other_settings_and_controls.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/core/dev_save_coordinator.hpp"
#include "devtools/core/save_manager.hpp"
#include "map_assets_modals.hpp"

class Asset;
class Input;
class Assets;
class WarpedScreenGrid;
class AssetInfo;
class Room;
class RoomEditor;
class MapEditor;
class MapModeUI;
class CameraUIPanel;
class RegenerateRoomPopup;

namespace animation_editor {
class AnimationDocument;
class PreviewProvider;
}

class DMButton;
class DMTextBox;
class DMNumericStepper;

class DevControls {
public:
    enum class Mode {
        RoomEditor,
        MapEditor
};

    enum class DropContentKind {
        None,
        SinglePng,
        Gif,
        MultiImages,
        PngFolder
    };

    DevControls(Assets* owner, int screen_w, int screen_h);
    ~DevControls();

    void set_input(Input* input);
    void set_player(Asset* player);
    void set_active_assets(std::vector<Asset*>& actives, std::uint64_t version);
    void set_screen_dimensions(int width, int height);
    void set_current_room(Room* room, bool force_refresh = false);
    void set_rooms(std::vector<Room*>* rooms, std::size_t generation = 0);

    void set_map_info(nlohmann::json* map_info);
    void set_map_context(nlohmann::json* map_info, const std::string& map_path);

    Room* resolve_current_room(Room* detected_room);

    void set_enabled(bool enabled);
    bool run_exit_save_sequence(const std::string& reason);
    bool is_enabled() const { return enabled_; }
    Mode mode() const { return mode_; }
    void sync_camera_tilt_override();

    void set_camera_override_for_testing(WarpedScreenGrid* camera_override);

    void update(const Input& input);
    void update_ui(const Input& input);
    void handle_sdl_event(const SDL_Event& event);
    void render_overlays(SDL_Renderer* renderer);

    void toggle_asset_library();
    void open_asset_library();
    void close_asset_library();
    bool is_asset_library_open() const;

    std::shared_ptr<AssetInfo> consume_selected_asset_from_library();

    void open_asset_info_editor(const std::shared_ptr<AssetInfo>& info);
    void open_asset_info_editor_for_asset(Asset* asset);
    void open_animation_editor_for_asset(const std::shared_ptr<AssetInfo>& info);
    void close_asset_info_editor();
    bool consume_escape_for_asset_editor_stack();
    bool is_asset_info_editor_open() const;
    std::uint64_t other_settings_state_version() const;

    void finalize_asset_drag(Asset* asset, const std::shared_ptr<AssetInfo>& info);

    [[nodiscard]] devmode::core::ManifestStore& manifest_store();
    [[nodiscard]] const devmode::core::ManifestStore& manifest_store() const;
    [[nodiscard]] devmode::core::DevSaveCoordinator& save_coordinator();
    [[nodiscard]] const devmode::core::DevSaveCoordinator& save_coordinator() const;
    void mark_map_dirty(devmode::core::DevSaveCoordinator::Priority priority =
                            devmode::core::DevSaveCoordinator::Priority::Debounced);

    void toggle_room_config();
    void close_room_config();
    bool is_room_config_open() const;


    void focus_camera_on_asset(Asset* asset, double height_factor = 0.8, int duration_steps = 0);

    void reset_click_state();
    void clear_selection();
    void purge_asset(Asset* asset);
    void set_world_mutation_in_progress(bool in_progress);

    void notify_spawn_group_config_changed(const nlohmann::json& entry);
    void notify_spawn_group_removed(const std::string& spawn_id);

    const std::vector<Asset*>& get_selected_assets() const;
    const std::vector<Asset*>& get_highlighted_assets() const;
    Asset* get_hovered_asset() const;

    void set_height_scale_factor(double factor);
    double get_height_scale_factor() const;

    void filter_active_assets(std::vector<Asset*>& assets) const;
    bool fog_visible() const;
    bool boundary_assets_visible() const;

    bool is_grid_overlay_enabled() const { return grid_overlay_enabled_; }
    bool is_snap_to_grid_enabled() const { return snap_to_grid_enabled_; }
    int  grid_cell_size_px() const { return grid_cell_size_px_; }
    Assets::DevGridOverlayContext dev_grid_overlay_context() const;
    std::vector<Assets::DevFloorProjectionMarker> floor_projection_markers_for_floor_pass();

    void begin_frame_editor_session(Asset* asset,
                                    std::shared_ptr<animation_editor::AnimationDocument> document,
                                    std::shared_ptr<animation_editor::PreviewProvider> preview,
                                    const std::string& animation_id,
                                    FrameEditorLaunchMode launch_mode,
                                    std::function<void(const std::string&)> on_host_closed);
    void end_frame_editor_session();
    bool is_frame_editor_session_active() const;
    bool is_runtime_light_editor_active() const;
    const Asset* frame_editor_target() const;

    struct DropPreviewState {
        bool active = false;
        bool valid = false;
        SDL_Point screen{0, 0};
        std::vector<std::filesystem::path> items;
    };

    struct DropImportRequest {
        DropContentKind kind = DropContentKind::None;
        std::vector<std::filesystem::path> files;
        std::filesystem::path folder;
        SDL_Point drop_screen{0, 0};
    };

    struct DropNameModal {
        bool visible = false;
        DropImportRequest request;
        std::unique_ptr<DMTextBox> name_box;
        std::unique_ptr<DMButton> create_button;
        std::unique_ptr<DMButton> cancel_button;
        SDL_Rect modal_rect{0, 0, 0, 0};
        SDL_Rect create_rect{0, 0, 0, 0};
        SDL_Rect cancel_rect{0, 0, 0, 0};
        bool create_pressed = false;
        bool cancel_pressed = false;
        std::string error;
    };

    struct DropChoiceModal {
        bool visible = false;
        DropImportRequest request;
        std::unique_ptr<DMButton> single_animation_button;
        std::unique_ptr<DMButton> multiple_assets_button;
        std::unique_ptr<DMButton> cancel_button;
        SDL_Rect modal_rect{0, 0, 0, 0};
        SDL_Rect single_rect{0, 0, 0, 0};
        SDL_Rect multiple_rect{0, 0, 0, 0};
        SDL_Rect cancel_rect{0, 0, 0, 0};
    };

    struct DropConflictModal {
        bool visible = false;
        std::string asset_name;
        std::unique_ptr<DMButton> skip_button;
        std::unique_ptr<DMButton> rename_button;
        SDL_Rect modal_rect{0, 0, 0, 0};
        SDL_Rect skip_rect{0, 0, 0, 0};
        SDL_Rect rename_rect{0, 0, 0, 0};
    };

    struct DropErrorPopup {
        bool visible = false;
        std::string message;
        std::unique_ptr<DMButton> ok_button;
        SDL_Rect modal_rect{0, 0, 0, 0};
        SDL_Rect ok_rect{0, 0, 0, 0};
    };

    struct MultiAssetImportState {
        struct Item {
            DropImportRequest request;
            std::string suggested_name;
            std::string error_message;
        };

        bool active = false;
        std::vector<Item> items;
        std::size_t index = 0;
        bool waiting_for_rename = false;
        std::size_t imported_count = 0;
    };

    bool can_use_room_editor_ui() const;
    void enter_map_editor_mode();
    void exit_map_editor_mode(bool focus_player, bool restore_previous_state);
    void handle_map_selection();
    void toggle_camera_panel();
    void close_camera_panel();
    void toggle_boundary_assets_modal();
    void open_boundary_assets_modal();
    void configure_header_button_sets();
    void sync_header_button_states();
    void open_misc_options_panel();
    void close_misc_options_panel();
    void toggle_misc_options_panel();
    bool is_misc_options_panel_open() const;
    Room* find_spawn_room() const;
    Room* choose_room(Room* preferred) const;
    bool is_pointer_over_dev_ui(int x, int y) const;
    void close_all_floating_panels();
    void maybe_update_mode_from_height();
    void open_regenerate_room_popup();
    bool is_modal_blocking_panels() const;
    void pulse_modal_header();
    void apply_header_suppression();
    void create_trail_template();

    void refresh_active_asset_filters();
    void reset_asset_filters();
    bool passes_asset_filters(Asset* asset) const;
    bool should_hide_assets_for_map_mode() const;
    void apply_camera_area_render_flag();
    void set_mode_from_header(int header_mode);
    void set_mode(Mode new_mode);
    void update_movement_debug_visibility();
    void rebuild_settings_schema();
    void apply_bool_setting(const char* id, bool value, bool sync_other_settings);
    void apply_int_setting(const char* id, int value, bool sync_other_settings);
    void apply_overlay_grid_resolution(int resolution, bool user_override, bool update_stepper, bool update_footer);
    void apply_grid_resolution_change(int resolution);
    void nudge_overlay_grid_resolution(int delta);
    void push_grid_resolution_toast(int resolution);
    void ensure_misc_options_widgets();
    void sync_misc_options_from_map_info();
    void layout_misc_options_panel();
    bool handle_misc_options_panel_event(const SDL_Event& event);
    void render_misc_options_panel(SDL_Renderer* renderer);
    int read_map_tile_size_or_default8() const;
    void write_map_tile_size(int resolution);
    SDL_Color read_map_color_or_default() const;
    void write_map_color(SDL_Color color);
    void apply_misc_map_color_change_from_ui();
    void restore_filter_hidden_assets() const;
    void mark_layout_dirty();
    void rebuild_layout_state();
    void update_header_and_footer_bounds();
    void ensure_layout_cache();
    void mark_dirty(std::uint32_t flags);
    bool has_dirty(std::uint32_t flags) const;
    void clear_dirty(std::uint32_t flags);
    bool handle_drop_event(const SDL_Event& event);
    void reset_drop_preview();
    void reset_drop_modal();
    void open_drop_modal(const DropImportRequest& request);
    bool handle_drop_modal_event(const SDL_Event& event);
    bool handle_drop_choice_modal_event(const SDL_Event& event);
    bool handle_drop_conflict_modal_event(const SDL_Event& event);
    bool handle_drop_error_popup_event(const SDL_Event& event);
    void render_drop_overlay(SDL_Renderer* renderer);
    void render_drop_modal(SDL_Renderer* renderer);
    void render_drop_choice_modal(SDL_Renderer* renderer);
    void render_drop_conflict_modal(SDL_Renderer* renderer);
    void render_drop_error_popup(SDL_Renderer* renderer);
    void render_import_busy_overlay(SDL_Renderer* renderer);
    void layout_drop_modal();
    void layout_drop_choice_modal();
    void layout_drop_conflict_modal();
    void layout_drop_error_popup();
    bool finalize_drop_creation(const std::string& desired_name);
    bool create_drop_asset(const std::string& asset_name,
                           const std::vector<std::filesystem::path>& files,
                           const DropImportRequest& request,
                           bool open_editor_and_spawn,
                           std::string& error_out);
    void open_drop_choice_modal(const DropImportRequest& request);
    void begin_multi_asset_import(const DropImportRequest& request);
    void begin_multi_folder_import(const std::vector<std::filesystem::path>& folders, SDL_Point drop_screen);
    void process_next_multi_asset_item();
    const MultiAssetImportState::Item* current_multi_asset_import_item() const;
    void open_drop_conflict_modal(const std::string& asset_name);
    void open_drop_error_popup(const std::string& message);
    void reset_drop_choice_modal();
    void reset_drop_conflict_modal();
    void reset_drop_error_popup();
    void reset_multi_asset_import();
    SDL_Point drop_world_from_screen(SDL_Point screen) const;

    void begin_import_busy(const std::string& message);
    void end_import_busy();
    bool is_import_busy() const;

private:
    enum class DirtyFlag : std::uint32_t {
        None   = 0,
        Layout = 1 << 0,
    };

    static constexpr std::uint32_t kDirtyLayout = static_cast<std::uint32_t>(DirtyFlag::Layout);

    struct LayoutCache {
        SDL_Rect usable_rect{0, 0, 0, 0};
        bool valid = false;
    };

    struct GridResolutionToast {
        std::string text;
        Uint64 start_ms = 0;
        Uint64 duration_ms = 0;
    };

    int map_radius_or_default() const;
    void remove_spawn_group_assets(const std::string& spawn_id);
    void integrate_spawned_assets(std::vector<std::unique_ptr<Asset>>& spawned);
    void regenerate_map_spawn_group(const nlohmann::json& entry);
    void regenerate_boundary_spawn_group(const nlohmann::json& boundary_data);
    
    void ensure_boundary_assets_modal_open();


    bool persist_map_info_to_disk();

    Assets* assets_ = nullptr;
    Input* input_ = nullptr;
    std::vector<Asset*>* active_assets_ = nullptr;
    std::uint64_t active_assets_version_ = 0;
    Asset* player_ = nullptr;
    Room* current_room_ = nullptr;
    Room* detected_room_ = nullptr;
    Room* dev_selected_room_ = nullptr;
    std::vector<Room*>* rooms_ = nullptr;
    std::size_t rooms_generation_ = 0;

    int screen_w_ = 0;
    int screen_h_ = 0;
    bool enabled_ = false;
    Mode mode_ = Mode::RoomEditor;

    std::unique_ptr<RoomEditor> room_editor_;
    std::unique_ptr<MapEditor> map_editor_;
    nlohmann::json* map_info_json_ = nullptr;
    std::function<bool()> map_grid_save_cb_;
    std::function<void()> map_grid_regen_cb_;
    std::unique_ptr<MapModeUI> map_mode_ui_;
    std::unique_ptr<CameraUIPanel> camera_panel_;
    std::unique_ptr<RegenerateRoomPopup> regenerate_popup_;
    std::string map_path_;
    bool pointer_over_camera_panel_ = false;
    bool modal_headers_hidden_ = false;
    bool sliding_headers_hidden_ = false;
    bool world_mutation_in_progress_ = false;
    bool pending_selection_sync_refresh_ = false;
    struct FilterHiddenAssetState {
        bool hidden = false;
        bool active = false;
    };
    mutable std::unordered_map<Asset*, FilterHiddenAssetState> filter_hidden_assets_;
    mutable std::unordered_set<Asset*> previous_filtered_membership_;
    
    
    devmode::core::ManifestStore manifest_store_;
    devmode::core::DevSaveCoordinator save_coordinator_;
    devmode::core::SaveManager save_manager_;
    bool exit_save_sequence_ran_ = false;
    bool exit_save_sequence_ok_ = true;
    bool map_dirty_ = false;
    bool map_info_dirty_ = false;
    OtherSettingsAndControls other_settings_;
    std::vector<OtherSettingsAndControls::SettingSchema> global_settings_schema_;

    WarpedScreenGrid* camera_override_for_testing_ = nullptr;

    std::unique_ptr<BoundarySpawnGroupModal> boundary_assets_modal_;

    bool grid_overlay_enabled_ = false;
    bool snap_to_grid_enabled_ = false;
      int  grid_overlay_resolution_r_ = 0;
      bool grid_overlay_resolution_user_override_ = false;
      int  grid_cell_size_px_ = 1;
    std::optional<GridResolutionToast> grid_resolution_toast_;
    int  grid_resolution_r_ = -1;
    bool movement_debug_enabled_ = false;
    bool anchor_point_debug_enabled_ = false;
    bool misc_options_panel_open_ = false;
    bool misc_options_panel_suppress_callbacks_ = false;
    SDL_Rect misc_options_panel_rect_{0, 0, 0, 0};
    std::unique_ptr<DMNumericStepper> misc_tile_size_stepper_;
    std::array<std::unique_ptr<DMNumericStepper>, 4> misc_map_color_steppers_{};
    SDL_Color misc_map_color_{0, 0, 0, 255};

    std::unique_ptr<class FrameEditorSession> frame_editor_session_;
    bool frame_editor_prev_grid_overlay_ = false;

    bool frame_editor_prev_asset_info_open_ = false;
    Asset* frame_editor_asset_for_reopen_ = nullptr;

    bool render_suppression_in_progress_ = false;
    bool shift_block_headers_footers_ = false;

    std::uint32_t dirty_flags_ = kDirtyLayout;
    LayoutCache layout_cache_;
    SDL_Rect last_header_rect_{0, 0, 0, 0};
    SDL_Rect last_footer_rect_{0, 0, 0, 0};

    DropPreviewState drop_state_;
    DropNameModal drop_modal_;
    DropChoiceModal drop_choice_modal_;
    DropConflictModal drop_conflict_modal_;
    DropErrorPopup drop_error_popup_;
    struct ImportBusyOverlay {
        bool active = false;
        std::string message;
        Uint64 started_ms = 0;
    } import_busy_;
    MultiAssetImportState multi_asset_import_;
    enum class DepthGuideSelection { None, RedCull, OrangeEfficiency, BlueLayer };
    DepthGuideSelection depth_guide_selection_ = DepthGuideSelection::None;
    bool depth_guide_drag_active_ = false;
    int depth_guide_drag_start_y_ = 0;
    WarpedScreenGrid::RealismSettings depth_guide_preview_settings_{};
    bool depth_guide_preview_active_ = false;
    Uint64 depth_guide_blue_wheel_last_change_ms_ = 0;


};
