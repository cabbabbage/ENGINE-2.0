#pragma once

#include <SDL3/SDL.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace devmode::core { class ManifestStore; }
namespace devmode::core { class ManifestStore; }

#include "SlidingWindowContainer.hpp"
#include "widgets.hpp"

class Input;
class Room;
class Assets;
class TagEditorWidget;
class DropdownWidget;
class SliderWidget;
class RangeSliderWidget;
class CheckboxWidget;
class TextBoxWidget;
class DockableCollapsible;
class DMSlider;
class DMCheckbox;
class DMTextBox;
class DMRangeSlider;
class DMDropdown;
class DMButton;
class DMNumericStepper;
class Widget;
class StepperWidget;
class ButtonWidget;
class DevColorPicker;

class RoomConfigurator {
public:
    struct SpawnGroupListItem {
        std::string spawn_id;
        std::string display_name;
        std::string icon_label;
    };

    RoomConfigurator();
    ~RoomConfigurator();

    void set_bounds(const SDL_Rect& bounds);
    void set_work_area(const SDL_Rect& bounds);
    void set_show_header(bool show);
    void set_on_close(std::function<void()> cb);
    void set_header_visibility_controller(std::function<void(bool)> cb);
    void set_blocks_editor_interactions(bool block);
    void attach_container(SlidingWindowContainer* container);
    void detach_container();
    SlidingWindowContainer* container();
    const SlidingWindowContainer* container() const;

    void open(const nlohmann::json& room_data);
    void open(nlohmann::json& room_data, std::function<void()> on_change = {});
    void open(Room* room);

    void set_manifest_store(class devmode::core::ManifestStore* store);
    void set_assets(Assets* assets);
    void set_room_save_callback(std::function<bool(bool immediate)> cb) { room_save_callback_ = std::move(cb); }
    void set_spawn_groups_provider(std::function<std::vector<SpawnGroupListItem>()> provider);
    void set_on_spawn_group_click(std::function<void(const std::string&)> cb);
    void set_on_spawn_group_double_click(std::function<void(const std::string&)> cb);
    void set_on_spawn_group_delete(std::function<bool(const std::string&)> cb);
    void set_on_generate_room(std::function<void(const std::string&)> cb) { on_generate_room_ = std::move(cb); }
    void set_room_metadata_only_mode(bool enabled);
    bool room_metadata_only_mode() const { return room_metadata_only_mode_; }

    void close();
    bool visible() const;
    bool any_panel_visible() const;
    bool is_locked() const;

    void update(const Input& input, int screen_w, int screen_h);
    void prepare_for_event(int screen_w, int screen_h);
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* r) const;

    const SDL_Rect& panel_rect() const;

    std::string current_header_text() const;

    nlohmann::json build_json() const;
    bool is_point_inside(int x, int y) const;

    bool focus_name_field();

    void set_on_room_renamed(std::function<std::string(const std::string&, const std::string&)> cb) {
        on_room_renamed_ = std::move(cb);
    }

    void refresh_camera_panel_widgets();
    void reload_camera_state_from_room();
    void request_camera_live_update();
    void set_on_camera_changed(std::function<void(Room*)> cb) { on_camera_changed_ = std::move(cb); }
    bool camera_controls_enabled() const;
    struct CameraAdjustment {
        int height_delta_px = 0;
        float tilt_delta_deg = 0.0f;
        int zoom_delta_percent = 0;
        float pan_delta_percent = 0.0f;
    };
    bool apply_camera_adjustment(const CameraAdjustment& adjustment);

private:
    class devmode::core::ManifestStore* manifest_store_ = nullptr;
    Assets* assets_ = nullptr;
    struct State;

    bool apply_room_data(const nlohmann::json& data);
    void rebuild_rows();
    void rebuild_rows_internal();

    void request_rebuild();
    void load_tags_from_json(const nlohmann::json& data);
    void write_tags_to_json(nlohmann::json& object) const;
    std::string selected_geometry() const;
    bool sync_state_from_widgets();
    const nlohmann::json& live_room_json() const;
    nlohmann::json& live_room_json();
    int layout_content(const SlidingWindowContainer::LayoutContext& ctx) const;
    SDL_Rect clamp_to_work_area(const SDL_Rect& bounds) const;
    void handle_container_closed();
    void reset_scroll();
    void ensure_base_panels();
    void refresh_base_panel_rows();
    void refresh_spawn_group_rows_if_needed();
    void request_container_layout();
    void configure_container(SlidingWindowContainer& container);
    void clear_container_callbacks(SlidingWindowContainer& container);
    void prune_collapsible_caches();
    int cached_collapsible_height(const DockableCollapsible* panel) const;
    void update_collapsible_height_cache(const DockableCollapsible* panel, int new_height);
    void forget_collapsible(const DockableCollapsible* panel);
    bool base_panel_expanded(const std::string& key) const;
    void set_base_panel_expanded(const std::string& key, bool expanded);
    void focus_panel(DockableCollapsible* panel);
    void clear_panel_focus();
    void apply_panel_focus_states();
    DockableCollapsible* panel_at_point(SDL_Point p) const;
    bool handle_panel_focus_event(const SDL_Event& e);
    void expand_width_slider_range_if_needed();
    void expand_height_slider_range_if_needed();

    std::unique_ptr<State> state_;
    std::unique_ptr<SlidingWindowContainer> default_container_;
    SlidingWindowContainer* container_ = nullptr;
    bool blocks_editor_interactions_ = true;
    bool show_header_ = true;
    SDL_Rect bounds_override_{0, 0, 0, 0};
    SDL_Rect work_area_{0, 0, 0, 0};
    bool has_bounds_override_ = false;
    int last_screen_w_ = 0;
    int last_screen_h_ = 0;
    std::function<void()> on_close_{};
    bool rebuild_in_progress_ = false;
    bool pending_rebuild_ = false;
    bool deferred_rebuild_ = false;

    Room* room_ = nullptr;
    nlohmann::json* external_room_json_ = nullptr;
    nlohmann::json loaded_json_;
    nlohmann::json metadata_snapshot_;
    std::size_t metadata_snapshot_hash_ = 0;
    bool is_trail_context_ = false;

    std::vector<std::string> geometry_options_;

    std::vector<std::string> room_tags_;
    std::vector<std::string> room_anti_tags_;
    bool tags_dirty_ = false;
    bool trail_connection_sector_dirty_ = false;
    bool room_floor_color_dirty_ = false;

    std::unique_ptr<DMTextBox> name_box_;
    std::unique_ptr<TextBoxWidget> name_widget_;
    std::unique_ptr<DMDropdown> geometry_dropdown_;
    std::unique_ptr<DropdownWidget> geometry_widget_;
    std::unique_ptr<DMRangeSlider> width_range_slider_;
    std::unique_ptr<RangeSliderWidget> width_range_widget_;
    std::unique_ptr<DMRangeSlider> height_range_slider_;
    std::unique_ptr<RangeSliderWidget> height_range_widget_;
    int width_slider_max_range_ = 0;
    int height_slider_max_range_ = 0;
    std::unique_ptr<DMSlider> edge_slider_;
    std::unique_ptr<SliderWidget> edge_widget_;
    std::unique_ptr<DMSlider> curvy_slider_;
    std::unique_ptr<SliderWidget> curvy_widget_;
    std::unique_ptr<Widget> trail_connection_sector_widget_;
    std::unique_ptr<DMNumericStepper> sector_direction_stepper_;
    std::unique_ptr<StepperWidget> sector_direction_widget_;
    std::unique_ptr<DMNumericStepper> sector_width_stepper_;
    std::unique_ptr<StepperWidget> sector_width_widget_;
    std::unique_ptr<DMButton> sector_reset_button_;
    std::unique_ptr<ButtonWidget> sector_reset_widget_;
    std::unique_ptr<DMCheckbox> boss_checkbox_;
    std::unique_ptr<CheckboxWidget> boss_widget_;
    std::unique_ptr<DMCheckbox> inherit_checkbox_;
    std::unique_ptr<CheckboxWidget> inherit_widget_;
    std::unique_ptr<DMCheckbox> inherit_floor_color_checkbox_;
    std::unique_ptr<CheckboxWidget> inherit_floor_color_widget_;
    std::unique_ptr<DMButton> room_floor_color_button_;
    std::unique_ptr<ButtonWidget> room_floor_color_widget_;
    std::unique_ptr<DevColorPicker> color_picker_;
    std::unique_ptr<TagEditorWidget> tag_editor_;

    std::unique_ptr<DockableCollapsible> geometry_panel_;
    std::unique_ptr<DockableCollapsible> tags_panel_;
    std::unique_ptr<DockableCollapsible> types_panel_;
    std::unique_ptr<DockableCollapsible> spawn_groups_panel_;
    std::vector<DockableCollapsible*> ordered_base_panels_;
    mutable std::vector<SDL_Rect> ordered_panel_bounds_;
    std::unordered_map<const DockableCollapsible*, int> collapsible_height_cache_;
    std::unordered_map<const DockableCollapsible*, std::string> base_panel_keys_;
    std::unordered_map<std::string, bool> base_panel_expanded_state_;
    DockableCollapsible* focused_panel_ = nullptr;

    bool reset_expanded_state_pending_ = false;

    std::function<void()> on_external_change_;
    std::function<bool(bool immediate)> room_save_callback_;
    std::function<std::string(const std::string&, const std::string&)> on_room_renamed_;
    std::function<void(bool)> header_visibility_controller_{};
    std::function<void(Room*)> on_camera_changed_;
    std::function<std::vector<SpawnGroupListItem>()> spawn_groups_provider_;
    std::function<void(const std::string&)> on_spawn_group_click_;
    std::function<void(const std::string&)> on_spawn_group_double_click_;
    std::function<bool(const std::string&)> on_spawn_group_delete_;
    std::function<void(const std::string&)> on_generate_room_;
    std::vector<SpawnGroupListItem> spawn_group_rows_{};
    std::size_t spawn_group_rows_hash_ = 0;
    std::unique_ptr<Widget> spawn_group_list_widget_;
    bool room_metadata_only_mode_ = false;
};
