#pragma once

#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include <nlohmann/json_fwd.hpp>

#include "devtools/SlidingWindowContainer.hpp"
#include "devtools/asset_info_sections.hpp"
#include "utils/ranged_color.hpp"
#include "devtools/core/dev_save_coordinator.hpp"

class AssetInfo;
class DockableCollapsible;
class DMCheckbox;
class DMDropdown;
class DMButton;
class DropdownWidget;
class CheckboxWidget;
class ButtonWidget;
class Input;
class Area;
class Assets;
class SearchAssets;
namespace animation_editor {
class AnimationEditorWindow;
class AnimationDocument;
}

namespace devmode::core {
class ManifestStore;
class DevSaveCoordinator;
}

class AssetInfoUI {

	public:
    AssetInfoUI();
    ~AssetInfoUI();
    void set_info(const std::shared_ptr<AssetInfo>& info);
    void clear_info();
    void open();
    void close();
    void toggle();
    bool is_visible() const { return visible_; }
    bool is_locked() const;
    void update(const Input& input, int screen_w, int screen_h);
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* r, int screen_w, int screen_h) const;
    void render_world_overlay(SDL_Renderer* r, const class WarpedScreenGrid& cam) const;
    void pulse_header();

    void open_animation_editor_panel();
    void set_assets(Assets* a);
    Assets* assets() const { return assets_; }
    void set_manifest_store(devmode::core::ManifestStore* store);
    void set_save_coordinator(devmode::core::DevSaveCoordinator* coordinator);
    devmode::core::DevSaveCoordinator* save_coordinator() const { return save_coordinator_; }
    devmode::core::ManifestStore* manifest_store() const { return manifest_store_; }
    void set_target_asset(class Asset* a);
    class Asset* get_target_asset() const { return target_asset_; }
    bool is_point_inside(int x, int y) const;
    SDL_Renderer* get_last_renderer() const { return last_renderer_; }
    void refresh_target_asset_scale();
    void sync_target_tiling_state();
    void request_apply_section(AssetInfoSectionId section_id);
    void set_header_visibility_callback(std::function<void(bool)> cb);
    void mark_target_asset_composite_dirty();
    void sync_target_spacing_settings();
    void sync_target_tags();
    void sync_target_basic_render_settings(bool type_changed);

    void begin_color_sampling(const utils::color::RangedColor& current, std::function<void(SDL_Color)> on_sample, std::function<void()> on_cancel);
    void cancel_color_sampling(bool silent);
    bool enqueue_manifest_save(devmode::core::DevSaveCoordinator::Priority priority,
                               const std::string& label,
                               std::function<void()> on_success = {});

  private:
    void rebuild_default_sections();
    void layout_widgets(int screen_w, int screen_h) const;
    void apply_camera_override(bool enable);
    float compute_player_screen_height(const class WarpedScreenGrid& cam) const;
    void save_now() const;
    bool apply_section_to_assets(AssetInfoSectionId section_id, const std::vector<std::string>& asset_names);
    static const char* section_display_name(AssetInfoSectionId section_id);
    bool validate_target_asset() const;
    bool apply_to_assets_with_info(const std::function<void(Asset*)>& fn);
    bool asset_matches_current_info(const Asset* asset) const;
    void on_animation_document_saved();
    void refresh_loaded_asset_instances();
    void complete_color_sampling(SDL_Color color);
    void focus_section(DockableCollapsible* section);
    void apply_section_focus_states();
    void clear_section_focus();
    DockableCollapsible* section_at_point(SDL_Point p) const;
    bool handle_section_focus_event(const SDL_Event& e);
    std::shared_ptr<animation_editor::AnimationDocument> animation_document() const;

  private:
    bool visible_ = false;
    std::shared_ptr<AssetInfo> info_{};
    mutable SDL_Renderer* last_renderer_ = nullptr;
    Assets* assets_ = nullptr;

    std::vector<std::unique_ptr<DockableCollapsible>> sections_;
    DockableCollapsible* focused_section_ = nullptr;
    DockableCollapsible* basic_info_section_ = nullptr;
    mutable std::vector<SDL_Rect> section_bounds_;

    mutable class Asset* target_asset_ = nullptr;
    mutable SDL_Rect animation_editor_rect_{0,0,0,0};
    int last_screen_w_ = 0;
    int last_screen_h_ = 0;

    SlidingWindowContainer container_;

    mutable std::unique_ptr<class DMButton> configure_btn_;
    mutable std::unique_ptr<class ButtonWidget> configure_btn_widget_;
    bool camera_override_active_ = false;
    bool prev_camera_realism_enabled_ = false;
    bool prev_camera_parallax_enabled_ = false;
    std::unique_ptr<SearchAssets> asset_selector_;
    std::unique_ptr<animation_editor::AnimationEditorWindow> animation_editor_window_;
    bool pending_animation_editor_open_ = false;
    bool forcing_high_quality_rendering_ = false;
    devmode::core::ManifestStore* manifest_store_ = nullptr;
    devmode::core::DevSaveCoordinator* save_coordinator_ = nullptr;
    std::unique_ptr<class DMButton> duplicate_btn_;
    std::unique_ptr<class ButtonWidget> duplicate_btn_widget_;
    std::unique_ptr<class DMButton> delete_btn_;
    std::unique_ptr<class ButtonWidget> delete_btn_widget_;

    bool showing_duplicate_popup_ = false;
    std::string duplicate_asset_name_;

    bool showing_delete_popup_ = false;
    struct PendingDeleteInfo { std::string name; std::string asset_dir; };
    std::optional<PendingDeleteInfo> pending_delete_;
    SDL_Rect delete_modal_rect_{0,0,0,0};
    SDL_Rect delete_yes_rect_{0,0,0,0};
    SDL_Rect delete_no_rect_{0,0,0,0};
    bool delete_yes_hovered_ = false;
    bool delete_no_hovered_ = false;
    bool delete_yes_pressed_ = false;
    bool delete_no_pressed_ = false;

    bool duplicate_current_asset(const std::string& new_name);
    void request_delete_current_asset();
    void cancel_delete_request();
    void confirm_delete_request();
    void clear_delete_state();
    bool handle_delete_modal_event(const SDL_Event& e);
    void update_delete_modal_geometry(int screen_w, int screen_h);


    bool color_sampling_active_ = false;
    bool color_sampling_preview_valid_ = false;
    SDL_Color color_sampling_preview_{255,255,255,255};
    SDL_Point color_sampling_cursor_{0,0};
    std::function<void(SDL_Color)> color_sampling_apply_{};
    std::function<void()> color_sampling_cancel_{};
    SDL_Cursor* color_sampling_prev_cursor_ = nullptr;
    SDL_Cursor* color_sampling_cursor_handle_ = nullptr;
};

