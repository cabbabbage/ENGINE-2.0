#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

class Asset;
class DMCheckbox;
class DMNumericStepper;
class Room;
class Assets;
class Input;

class OtherSettingsAndControls {
public:
    using StateChangedCallback = std::function<void()>;
    using ExtraRenderer = std::function<void(SDL_Renderer*, const SDL_Rect&)>;
    using ExtraEventHandler = std::function<bool(const SDL_Event&, const SDL_Rect&)>;
    struct ModeButtonConfig {
        std::string id;
        std::string label;
        bool active = false;
};

    enum class SettingGroup { Grid, Debug, Overlay };
    enum class SettingControl { Toggle, Stepper };

    struct SettingSchema {
        std::string id;
        std::string label;
        SettingGroup group = SettingGroup::Grid;
        SettingControl control = SettingControl::Toggle;
        int min_value = 0;
        int max_value = 0;
        std::function<bool()> bool_getter;
        std::function<void(bool)> bool_setter;
        std::function<int()> int_getter;
        std::function<void(int)> int_setter;
};

    static constexpr const char* kShowGridSettingId = "show_grid";
    static constexpr const char* kSnapToGridSettingId = "snap_to_grid";
    static constexpr const char* kMovementDebugSettingId = "movement_debug";
    static constexpr const char* kAnchorPointDebugSettingId = "anchor_point_debug";
    static constexpr const char* kDepthEffectsSettingId = "depth_effects";
    static constexpr const char* kOverlayResolutionSettingId = "overlay_resolution";

    OtherSettingsAndControls();
    ~OtherSettingsAndControls();

    void initialize();

    void set_state_changed_callback(StateChangedCallback cb);
    std::uint64_t state_version() const { return state_version_; }
    void set_enabled(bool enabled);
    void set_screen_dimensions(int width, int height);
    void set_map_info(nlohmann::json* map_info);
    void set_current_room(Room* room);
    void set_header_title(const std::string& title);
    void set_mode_buttons(std::vector<ModeButtonConfig> buttons);
    void set_mode_changed_callback(std::function<void(const std::string&)> cb);
    void set_active_mode(const std::string& id, bool trigger_callback = false);
    void set_filters_expanded(bool expanded);
    bool filters_expanded() const { return filters_expanded_; }
    void set_settings_schema(std::vector<SettingSchema> schema);
    void refresh_setting_values();
    void set_setting_value(const std::string& id, bool enabled);
    void set_setting_value(const std::string& id, int value);

    void set_header_suppressed(bool suppressed);
    bool header_suppressed() const { return header_suppressed_; }

    void update(const Input& input);
    void refresh_layout();
    void ensure_layout();

    void render(SDL_Renderer* renderer) const;
    bool handle_event(const SDL_Event& event);
    bool contains_point(int x, int y) const;

    const SDL_Rect& header_rect() const { return header_rect_; }
    const SDL_Rect& layout_bounds() const { return layout_bounds_; }

    void set_right_accessory_width(int width) { right_accessory_width_ = std::max(0, width); layout_dirty_ = true; }
    int right_accessory_width() const { return right_accessory_width_; }

    void set_extra_panel_height(int height) { extra_panel_height_ = std::max(0, height); layout_dirty_ = true; }
    void set_extra_panel_renderer(ExtraRenderer renderer) { extra_renderer_ = std::move(renderer); }
    void set_extra_panel_event_handler(ExtraEventHandler handler) { extra_event_handler_ = std::move(handler); }
    void set_assets_context(Assets* assets) { assets_context_ = assets; }

    void reset();

    bool passes(const Asset& asset) const;
    bool is_type_filter_enabled(const std::string& type) const;

    void set_grid_resolution_range(int min_resolution, int max_resolution);
    void set_grid_resolution_value(int resolution);
    int grid_resolution_value() const;
    void set_grid_resolution_change_callback(std::function<void(int)> cb);

    bool fog_visible() const;

private:
    enum class FilterKind { MapAssets, CurrentRoom, Fog, Type };

    struct FilterEntry {
        std::string id;
        FilterKind kind = FilterKind::Type;
        std::unique_ptr<DMCheckbox> checkbox;
};

    struct FilterState {
        bool map_assets = false;
        bool current_room = true;
        bool fog = true;
        std::unordered_map<std::string, bool> type_filters;
};

    void rebuild_map_spawn_ids();
    void rebuild_room_spawn_ids();
    void rebuild_layout();
    void sync_state_from_ui();
    void load_persisted_state();
    void persist_state();
    void persist_filters_expanded() const;
    void ensure_dev_settings_controls();
    void layout_dev_settings();
    struct SettingWidget;
    SettingWidget* find_setting(const std::string& id);
    FilterState& mutable_state();
    const FilterState& state() const;
    void notify_state_changed();
    bool type_filter_enabled(const std::string& type) const;
    bool default_type_enabled(const std::string& type) const;
    bool load_type_filter_value(const std::string& type, bool default_value) const;
    std::string format_type_label(const std::string& type) const;
    void collect_spawn_ids(const nlohmann::json& node, std::unordered_set<std::string>& out) const;
    void update_filter_toggle_label();
    void clear_checkbox_rects();
    void layout_mode_buttons();
    void layout_filter_checkboxes();
    void layout_stats_panel();
    void maybe_refresh_stats(SDL_Renderer* renderer);
    void render_stats(SDL_Renderer* renderer) const;
    void shift_all_by(int dy);
    int hidden_offset_y() const;
    void begin_slide(bool hidden, Uint64 now_ms);
    void update_slide(Uint64 now_ms);
    void request_hidden_state(bool hidden, Uint64 now_ms, bool bypass_debounce);

    static FilterState& persistent_state();
    static bool& persistent_state_initialized_flag();
    static bool& persistent_state_loaded_flag();
    static bool& persistent_filters_expanded_flag();
    static void ensure_persistent_state_loaded();

    bool enabled_ = true;
    int screen_w_ = 0;
    int screen_h_ = 0;
    nlohmann::json* map_info_json_ = nullptr;
    Room* current_room_ = nullptr;

    std::vector<FilterEntry> entries_;
    FilterState* state_ = nullptr;
    bool has_saved_state_ = false;
    SDL_Rect layout_bounds_{0, 0, 0, 0};
    SDL_Rect mode_bar_rect_{0, 0, 0, 0};
    SDL_Rect header_rect_{0, 0, 0, 0};
    SDL_Rect settings_rect_{0, 0, 0, 0};
    SDL_Rect settings_heading_rect_{0, 0, 0, 0};
    SDL_Rect grid_section_label_rect_{0, 0, 0, 0};
    SDL_Rect debug_section_label_rect_{0, 0, 0, 0};
    SDL_Rect overlay_section_label_rect_{0, 0, 0, 0};
    SDL_Rect filters_heading_rect_{0, 0, 0, 0};
    SDL_Rect primary_filters_heading_rect_{0, 0, 0, 0};
    SDL_Rect advanced_filters_heading_rect_{0, 0, 0, 0};
    SDL_Rect grid_resolution_label_rect_{0, 0, 0, 0};
    SDL_Rect filters_rect_{0, 0, 0, 0};
    SDL_Rect header_title_rect_{0, 0, 0, 0};
    SDL_Rect stats_rect_{0, 0, 0, 0};
    SDL_Rect stats_heading_rect_{0, 0, 0, 0};
    std::array<SDL_Rect, 4> stats_line_rects_{};
    bool layout_dirty_ = true;
    std::unordered_set<std::string> map_spawn_ids_;
    std::unordered_set<std::string> room_spawn_ids_;
    StateChangedCallback on_state_changed_{};
    std::uint64_t state_version_ = 1;
    struct ModeButtonEntry {
        ModeButtonConfig config;
        std::unique_ptr<class DMButton> button;
};
    std::vector<ModeButtonEntry> mode_buttons_;
    std::string header_title_;
    std::string header_title_display_;
    std::function<void(const std::string&)> on_mode_selected_{};
    std::unique_ptr<class DMButton> filter_toggle_button_;
    std::unique_ptr<class DMButton> hide_button_;
    SDL_Rect hide_button_rect_{0, 0, 0, 0};
    bool filters_expanded_ = false;
    bool header_suppressed_ = false;
    bool auto_hidden_ = true;
    bool manual_hidden_lock_ = false;
    bool slide_active_ = false;
    int layout_offset_y_ = 0;
    int slide_start_y_ = 0;
    int slide_target_y_ = 0;
    Uint64 slide_started_ms_ = 0;
    bool debounce_pending_ = false;
    bool debounce_hidden_target_ = true;
    Uint64 debounce_started_ms_ = 0;
    int right_accessory_width_ = 0;
    int extra_panel_height_ = 0;
    SDL_Rect extra_panel_rect_{0,0,0,0};
    ExtraRenderer extra_renderer_{};
    ExtraEventHandler extra_event_handler_{};
    std::unique_ptr<DMNumericStepper> grid_resolution_stepper_;
    struct SettingWidget {
        SettingSchema schema;
        std::unique_ptr<DMCheckbox> checkbox;
        std::unique_ptr<DMNumericStepper> stepper;
    };
    std::vector<SettingWidget> settings_;
    std::function<void(int)> on_grid_resolution_changed_;
    int grid_resolution_min_ = 0;
    int grid_resolution_max_ = 0;
    Assets* assets_context_ = nullptr;
    struct RuntimeStats {
        double cpu_usage_percent = 0.0;
        double fps = 0.0;
        double ram_used_gb = 0.0;
        double ram_total_gb = 0.0;
        Uint64 last_sample_ms = 0;
        std::array<std::string, 4> lines{};
        std::string cpu_name;
        std::string gpu_name;
    };
    RuntimeStats stats_{};
    Uint64 stats_refresh_ms_ = 7000;
    Uint64 last_cpu_sample_ms_ = 0;
#ifdef _WIN32
    unsigned long long last_cpu_kernel_time_ = 0;
    unsigned long long last_cpu_user_time_ = 0;
#endif
};
