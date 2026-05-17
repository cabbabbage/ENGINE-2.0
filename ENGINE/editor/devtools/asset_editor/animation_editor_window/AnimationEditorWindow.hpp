#pragma once

#include <SDL3/SDL.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>

#include "devtools/core/manifest_store.hpp"
#include "devtools/core/dev_save_coordinator.hpp"
#include "devtools/core/save_orchestrator.hpp"
#include "devtools/widgets.hpp"
#include "core/AssetsManager.hpp"

#include <nlohmann/json_fwd.hpp>

#ifndef FRAME_EDITOR_ACCESS
#define FRAME_EDITOR_ACCESS private
#endif

class Asset;
class Assets;
class AssetInfo;
class Input;
class DMButton;
class DMTextBox;

namespace animation_editor {

using ::Asset;
using ::Assets;
using ::AssetInfo;

class AnimationDocument;
class AnimationListPanel;
class AnimationInspectorPanel;
class PreviewProvider;
class AsyncTaskQueue;
class AudioImporter;
class AnimationListContextMenu;
class CustomControllerService;

using DMButton = ::DMButton;
using DMCheckbox = ::DMCheckbox;
using DMDropdown = ::DMDropdown;
using DMTextBox = ::DMTextBox;

class AnimationEditorWindow {
  public:
    enum class ExternalActionType {
        None,
        AddAnimation,
        Controller,
        CreateDefaults,
    };

    enum class ModalLifecycleState {
        Closed,
        OpenPendingLayout,
        OpenReady,
        Closing,
    };

    struct LoadDiagnostics {
        std::string manifest_key_resolution_result = "not_attempted";
        std::string manifest_transaction_open_result = "not_attempted";
        std::string snapshot_source_chosen = "none";
        int animation_count_loaded = 0;
        bool seeded_default_applied = false;
        int parse_failures = 0;
        int normalization_failures = 0;
    };

    AnimationEditorWindow();
    ~AnimationEditorWindow();

    void set_visible(bool visible, bool process_close = true);
    bool is_visible() const { return visible_; }
    void toggle_visible();

    void set_bounds(const SDL_Rect& bounds);
    const SDL_Rect& bounds() const { return bounds_; }

    void set_info(const std::shared_ptr<AssetInfo>& info);
    void clear_info();

    void set_manifest_store(devmode::core::ManifestStore* store);

    void update(const Input& input, int screen_w, int screen_h);
    void render(SDL_Renderer* renderer) const;
    bool handle_event(const SDL_Event& e);
    void focus_animation(const std::string& animation_id);

    void set_on_document_saved(std::function<void()> callback);
    void set_on_closed(std::function<void()> callback);
    void set_on_animation_properties_changed(std::function<void(const std::string&, const nlohmann::json&)> callback);

    std::shared_ptr<AnimationDocument> document() const { return document_; }

    void set_assets(Assets* assets) { assets_ = assets; }
    void set_target_asset(Asset* asset) { target_asset_ = asset; }
    void set_save_coordinator(devmode::core::DevSaveCoordinator* coordinator) { save_coordinator_ = coordinator; }
    void set_parent_window(SDL_Window* window) { parent_window_ = window; }
    bool trigger_add_animation_action();
    bool trigger_controller_action();
    bool trigger_create_defaults_action();
    std::string controller_action_label() const;
    bool is_layout_valid() const;
    bool is_ready_for_action_execution() const;

  FRAME_EDITOR_ACCESS:
    struct LayoutState {
        SDL_Rect list_rect{0, 0, 0, 0};
        SDL_Rect inspector_rect{0, 0, 0, 0};
        SDL_Rect status_rect{0, 0, 0, 0};
        SDL_Rect load_details_rect{0, 0, 0, 0};
        bool bounds_valid = false;
        bool list_valid = false;
        bool inspector_valid = false;
        bool status_valid = false;
    };

    struct PendingExternalAction {
        ExternalActionType type = ExternalActionType::None;
        std::uint64_t request_revision = 0;
        std::uint64_t first_seen_frame = 0;

        bool active() const { return type != ExternalActionType::None; }
        void clear() {
            type = ExternalActionType::None;
            request_revision = 0;
            first_seen_frame = 0;
        }
    };

    void handle_document_saved();
    void layout_children();
    LayoutState compute_layout_state() const;
    void apply_layout_state(const LayoutState& state);
    int status_height() const;
    void ensure_layout() const;
    void invalidate_inspector_background_cache();
    void configure_list_panel();
    void configure_inspector_panel();
    void refresh_panels_after_load();
    void select_animation(const std::optional<std::string>& animation_id, bool from_user);
    void ensure_selection_valid();
    void handle_list_context_menu(const std::string& animation_id, const SDL_Point& location);
    void prompt_rename_animation(const std::string& animation_id);
    void set_animation_as_start(const std::string& animation_id);
    void duplicate_animation(const std::string& animation_id);
    void delete_animation_with_confirmation(const std::string& animation_id);
    void render_background(SDL_Renderer* renderer) const;
    void render_status(SDL_Renderer* renderer) const;
    void render_load_diagnostics(SDL_Renderer* renderer) const;
    std::string format_load_diagnostics_json() const;
    void render_inspector(SDL_Renderer* renderer) const;
    void render_inspector_background(SDL_Renderer* renderer) const;
    bool handle_status_event(const SDL_Event& e);
    bool handle_defaults_modal_event(const SDL_Event& e);
    void set_status_message(const std::string& message, int frames = 300);
    void render_defaults_modal(SDL_Renderer* renderer) const;
    void layout_defaults_modal();
    void open_defaults_modal();
    bool try_open_defaults_modal(bool from_retry);
    bool defaults_modal_bounds_valid() const;
    bool defaults_modal_actionable() const;
    void maybe_retry_deferred_defaults_modal();
    void close_defaults_modal();
    void ensure_defaults_modal_widgets();
    void handle_pick_defaults_base_frames();
    void handle_create_defaults();
    bool ensure_animation_exists(const std::string& animation_id);
    bool create_or_replace_animation_payload(const std::string& animation_id, const nlohmann::json& payload);
    std::optional<int> parse_defaults_total_movement() const;
    bool defaults_base_faces_right() const;
    bool copy_frames_to_animation_folder(const std::string& animation_id,
                                         const std::vector<std::filesystem::path>& frames);
    bool remove_animation_source_folder(const std::string& animation_id, std::string& error_message);
    bool invalidate_asset_cache_now(std::string& error_message);
    nlohmann::json build_file_sourced_movement_payload(const std::string& animation_id,
                                                       int frame_count,
                                                       int dx,
                                                       int dy,
                                                       int dz,
                                                       const std::vector<std::string>& tags) const;
    nlohmann::json build_derived_movement_payload(const std::string& animation_id,
                                                  const std::string& source_animation_id,
                                                  int frame_count,
                                                  bool invert_x,
                                                  bool invert_y,
                                                  bool invert_z,
                                                  bool invert_frames_horizontal,
                                                  bool invert_frames_vertical,
                                                  const std::vector<std::string>& tags) const;
    void create_animation_via_prompt();
    void reload_document();
    void process_auto_save();
    void close_manifest_transaction();
    bool persist_manifest_payload(const nlohmann::json& payload, bool finalize = false);
    bool orchestrated_save(devmode::core::SaveOrchestrator::Reason reason,
                           const std::string& document_id,
                           const std::function<bool()>& write);
    std::optional<std::string> resolve_manifest_key(const AssetInfo& info) const;

    std::optional<std::filesystem::path> pick_folder() const;
    std::optional<std::filesystem::path> pick_gif() const;
    std::vector<std::filesystem::path> pick_png_sequence() const;
    std::optional<std::string> pick_animation_reference() const;
    std::optional<std::filesystem::path> pick_audio_file() const;

    void handle_controller_button_click();
    void update_controller_button_label();
    bool does_controller_exist() const;
    std::string sanitize_asset_name(const std::string& name) const;
    std::string generate_controller_key(const std::string& asset_name) const;
    std::string generate_class_name(const std::string& asset_name) const;
    std::vector<std::string> collect_available_animation_ids() const;
    std::string build_controller_metadata(const std::string& controller_key) const;
    bool write_or_update_controller_metadata(const std::filesystem::path& path, const std::string& metadata) const;
    void ensure_controller_factory_registration(const std::string& key, const std::string& class_name) const;
    void add_controller();
    void open_controller();
    void refresh_inspector_animation_callback();
    std::string normalize_animation_name(std::string_view raw) const;
    void log_ui_transition(const char* tag, const std::string& detail) const;
    void sync_modal_lifecycle_with_layout();
    void queue_external_action(ExternalActionType type);
    bool execute_external_action(ExternalActionType type);
    bool flush_pending_external_action();

  FRAME_EDITOR_ACCESS:
    bool visible_ = false;
    SDL_Rect bounds_{0, 0, 0, 0};
    std::weak_ptr<AssetInfo> info_;
    std::filesystem::path asset_root_path_;
    std::shared_ptr<AnimationDocument> document_;
    std::shared_ptr<PreviewProvider> preview_provider_;
    std::shared_ptr<AsyncTaskQueue> task_queue_;
    std::shared_ptr<AudioImporter> audio_importer_;
    std::unique_ptr<AnimationListPanel> list_panel_;
    std::unique_ptr<AnimationInspectorPanel> inspector_panel_;
    std::unique_ptr<AnimationListContextMenu> list_context_menu_;
    std::unique_ptr<DMButton> load_details_toggle_button_;
    std::unique_ptr<DMButton> load_details_copy_button_;
    bool defaults_modal_visible_ = false;
    ModalLifecycleState defaults_modal_lifecycle_ = ModalLifecycleState::Closed;
    std::unique_ptr<DMCheckbox> defaults_diagonals_checkbox_;
    std::unique_ptr<DMCheckbox> defaults_basic_movement_checkbox_;
    std::unique_ptr<DMCheckbox> defaults_elevation_checkbox_;
    std::unique_ptr<DMCheckbox> defaults_3d_diagonals_checkbox_;
    std::unique_ptr<DMCheckbox> defaults_base_faces_right_checkbox_;
    std::unique_ptr<DMTextBox> defaults_distance_box_;
    std::unique_ptr<DMButton> defaults_base_frames_button_;
    std::unique_ptr<DMButton> defaults_create_button_;
    std::unique_ptr<DMButton> defaults_cancel_button_;
    std::vector<std::filesystem::path> defaults_base_frame_paths_;
    SDL_Rect defaults_modal_rect_{0, 0, 0, 0};
    SDL_Rect defaults_modal_scroll_rect_{0, 0, 0, 0};
    int defaults_modal_scroll_offset_ = 0;
    int defaults_modal_scroll_max_ = 0;
    std::string defaults_modal_open_warning_;
    bool defaults_modal_open_deferred_ = false;
    SDL_Rect list_rect_{0, 0, 0, 0};
    SDL_Rect inspector_rect_{0, 0, 0, 0};
    SDL_Rect status_rect_{0, 0, 0, 0};
    SDL_Rect load_details_rect_{0, 0, 0, 0};
    bool load_details_expanded_ = false;
    LoadDiagnostics load_diagnostics_{};
    std::string status_message_;
    int status_timer_frames_ = 0;
    mutable SDL_Texture* inspector_background_cache_ = nullptr;
    mutable SDL_Rect inspector_background_cache_rect_{0, 0, 0, 0};
    mutable bool inspector_background_dirty_ = true;
    std::optional<std::string> selected_animation_id_;
    mutable bool layout_dirty_ = true;
    LayoutState layout_state_{};
    PendingExternalAction pending_external_action_{};
    std::uint64_t next_external_action_revision_ = 0;
    std::uint64_t frame_counter_ = 0;
    bool auto_save_pending_ = false;
    int auto_save_timer_frames_ = 0;
    std::function<void()> on_document_saved_;
    std::function<void()> on_closed_;
    std::function<void(const std::string&, const nlohmann::json&)> external_animation_properties_changed_;
    devmode::core::ManifestStore* manifest_store_ = nullptr;
    devmode::core::ManifestStore::AssetTransaction manifest_transaction_;
    std::string manifest_asset_key_;
    bool using_manifest_store_ = false;
    devmode::core::DevSaveCoordinator* save_coordinator_ = nullptr;
    devmode::core::SaveOrchestrator save_orchestrator_;

    Assets* assets_ = nullptr;
    Asset* target_asset_ = nullptr;
    SDL_Window* parent_window_ = nullptr;
    std::unique_ptr<CustomControllerService> custom_controller_service_;
    std::function<std::vector<std::filesystem::path>()> png_sequence_picker_override_;
    std::function<std::optional<std::string>(const std::string&, const std::string&, const std::string&)> text_prompt_override_;
    std::function<std::optional<int>(const std::string&, const std::string&, const std::vector<int>&)> choice_prompt_override_;
    std::function<bool(const std::string&, const std::vector<std::filesystem::path>&)> defaults_copy_frames_override_;
    std::function<bool(const nlohmann::json&, bool)> defaults_persist_manifest_override_;
    bool defaults_force_source_dependency_failure_ = false;
    bool defaults_force_payload_write_failure_ = false;

};

}
