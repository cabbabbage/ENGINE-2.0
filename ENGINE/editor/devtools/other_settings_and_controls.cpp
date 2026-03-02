#include "other_settings_and_controls.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "assets/Asset.hpp"
#include "assets/asset_filter_tags.hpp"
#include "assets/asset/asset_types.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/dev_ui_settings.hpp"
#include "devtools/dm_icons.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/widgets.hpp"
#include "devtools/font_cache.hpp"
#include "utils/input.hpp"
#include "gameplay/map_generation/room.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#endif

namespace {
constexpr int kToggleButtonMinWidth = 36;
constexpr int kHideButtonMinWidth = 36;
constexpr int kPanelOutlineThickness = 1;
constexpr int kSectionHeaderSpacing = 4;
constexpr const char* kDevSettingsTitle = "Dev Mode Settings";
constexpr const char* kFiltersTitle = "Asset Filters";
constexpr const char* kGridSectionTitle = "Grid Overlay & Snapping";
constexpr const char* kDebugSectionTitle = "Debug & Effects";
constexpr const char* kOverlaySectionTitle = "Overlay Resolution";
constexpr const char* kPrimaryFiltersTitle = "Visibility Filters";
constexpr const char* kAdvancedFiltersTitle = "Asset & Spawn Filters";
constexpr const char* kGridResolutionTitle = "Tile Resolution";
constexpr const char* kStatsTitle = "Runtime Stats";
constexpr Uint64 kStatsRefreshMs = 7000;
constexpr Uint64 kHeaderSlideDurationMs = 88;
constexpr Uint64 kHeaderZoneDebounceMs = 36;
constexpr float kHeaderShowZoneRatio = 0.10f;
constexpr float kHeaderUnlockZoneRatio = 0.20f;

constexpr const char* kSettingsInitializedKey = "dev.asset_filter.initialized";
constexpr const char* kSettingsMapAssetsKey = "dev.asset_filter.map_assets";
constexpr const char* kSettingsCurrentRoomKey = "dev.asset_filter.current_room";
constexpr const char* kSettingsFogKey = "dev.asset_filter.fog";
constexpr const char* kSettingsFiltersExpandedKey = "dev.asset_filter.filters_expanded";
constexpr const char* kSettingsMethodPrefix = "dev.asset_filter.methods.";

std::string make_type_setting_key(const std::string& type) {
    std::string canonical = asset_types::canonicalize(type);
    std::string key = "dev.asset_filter.types.";
    key += canonical;
    return key;
}

std::string make_method_setting_key(const std::string& method) {
    std::string key = kSettingsMethodPrefix;
    key += asset_filters::canonicalize_spawn_method(method);
    return key;
}

double bytes_to_gb(std::uint64_t bytes) {
    constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
    return bytes <= 0 ? 0.0 : static_cast<double>(bytes) / kGb;
}

std::string format_number(double value, int precision = 1) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

std::string format_percent(double value) {
    return format_number(value, 1) + "%";
}

std::string format_fps(double fps) {
    if (fps <= 0.01) {
        return std::string("N/A");
    }
    std::ostringstream oss;
    if (fps >= 100.0) {
        oss << std::fixed << std::setprecision(0) << fps;
    } else {
        oss << std::fixed << std::setprecision(1) << fps;
    }
    return oss.str();
}

SDL_Color reduced_alpha_color(const SDL_Color& color) {
    SDL_Color result = color;
    const int alpha = (static_cast<int>(result.a) + 1) / 2;
    result.a = static_cast<Uint8>(std::clamp(alpha, 0, 255));
    return result;
}

std::string format_ram_line(double used_gb, double total_gb) {
    if (used_gb <= 0.0 || total_gb <= 0.0) {
        return std::string("RAM: N/A");
    }
    std::ostringstream oss;
    oss << "RAM: " << format_number(used_gb, 1) << " / " << format_number(total_gb, 1) << " GB";
    return oss.str();
}

#ifdef _WIN32
unsigned long long filetime_to_ull(const FILETIME& ft) {
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string cpu_brand_string() {
    int cpu_info[4] = {0};
    __cpuid(cpu_info, 0x80000000);
    unsigned int n_ex_ids = static_cast<unsigned int>(cpu_info[0]);
    if (n_ex_ids < 0x80000004) {
        return {};
    }
    char brand[49] = {};
    __cpuid(cpu_info, 0x80000002);
    std::memcpy(brand, cpu_info, sizeof(cpu_info));
    __cpuid(cpu_info, 0x80000003);
    std::memcpy(brand + 16, cpu_info, sizeof(cpu_info));
    __cpuid(cpu_info, 0x80000004);
    std::memcpy(brand + 32, cpu_info, sizeof(cpu_info));
    brand[48] = '\0';
    std::string result(brand);
    // Collapse duplicate spaces for a cleaner label.
    std::string cleaned;
    cleaned.reserve(result.size());
    bool prev_space = false;
    for (char ch : result) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!prev_space) {
                cleaned.push_back(' ');
            }
            prev_space = true;
        } else {
            cleaned.push_back(ch);
            prev_space = false;
        }
    }
    while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.back()))) {
        cleaned.pop_back();
    }
    while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.front()))) {
        cleaned.erase(cleaned.begin());
    }
    return cleaned;
}
#endif

std::string fallback_cpu_label() {
#ifdef _WIN32
    const std::string brand = cpu_brand_string();
    if (!brand.empty()) {
        return brand;
    }
#endif
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        cores = 1;
    }
    std::ostringstream oss;
    oss << cores << "-core CPU";
    return oss.str();
}

std::string renderer_description(SDL_Renderer* renderer) {
    if (renderer) {
        if (SDL_PropertiesID props = SDL_GetRendererProperties(renderer)) {
            if (auto* gpu_device = static_cast<SDL_GPUDevice*>(
                    SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr))) {
                if (SDL_PropertiesID gpu_props = SDL_GetGPUDeviceProperties(gpu_device)) {
                    if (const char* gpu_name = SDL_GetStringProperty(gpu_props, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr)) {
                        return std::string(gpu_name);
                    }
                    if (const char* gpu_driver = SDL_GetStringProperty(gpu_props, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, nullptr)) {
                        return std::string(gpu_driver);
                    }
                }
            }
            if (const char* renderer_name = SDL_GetStringProperty(props, SDL_PROP_RENDERER_NAME_STRING, nullptr)) {
                return std::string(renderer_name);
            }
        }
        if (const char* renderer_name = SDL_GetRendererName(renderer)) {
            return std::string(renderer_name);
        }
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver) {
        return std::string(driver);
    }
    return {};
}

float smoothstep(float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - (2.0f * clamped));
}

std::string ellipsize_text(const std::string& text, int max_width, const DMLabelStyle& style) {
    if (max_width <= 0) {
        return std::string{};
    }
    SDL_Point full = MeasureLabelText(style, text);
    if (full.x <= max_width) {
        return text;
    }
    static const std::string kEllipsis = "...";
    SDL_Point ellipsis_size = MeasureLabelText(style, kEllipsis);
    if (ellipsis_size.x > max_width) {
        return std::string{};
    }
    std::string result = text;
    while (!result.empty()) {
        result.pop_back();
        SDL_Point trial = MeasureLabelText(style, result + kEllipsis);
        if (trial.x <= max_width) {
            return result + kEllipsis;
        }
    }
    return text;
}

}

OtherSettingsAndControls::FilterState& OtherSettingsAndControls::persistent_state() {
    static FilterState state{};
    return state;
}

bool& OtherSettingsAndControls::persistent_state_initialized_flag() {
    static bool initialized = false;
    return initialized;
}

bool& OtherSettingsAndControls::persistent_state_loaded_flag() {
    static bool loaded = false;
    return loaded;
}

bool& OtherSettingsAndControls::persistent_filters_expanded_flag() {
    static bool expanded = false;
    return expanded;
}

void OtherSettingsAndControls::ensure_persistent_state_loaded() {
    if (persistent_state_loaded_flag()) {
        return;
    }
    persistent_state_loaded_flag() = true;
    persistent_state_initialized_flag() = devmode::ui_settings::load_bool(kSettingsInitializedKey, false);
    if (!persistent_state_initialized_flag()) {
        FilterState& state = persistent_state();
        state.map_assets = true;
        state.current_room = true;
        state.fog = true;
        persistent_filters_expanded_flag() = false;
        return;
    }
    FilterState& state = persistent_state();
    state.map_assets = devmode::ui_settings::load_bool(kSettingsMapAssetsKey, true);
    state.current_room = devmode::ui_settings::load_bool(kSettingsCurrentRoomKey, true);
    state.fog = devmode::ui_settings::load_bool(kSettingsFogKey, true);
    persistent_filters_expanded_flag() = devmode::ui_settings::load_bool(kSettingsFiltersExpandedKey, false);
}

OtherSettingsAndControls::OtherSettingsAndControls() = default;
OtherSettingsAndControls::~OtherSettingsAndControls() = default;

void OtherSettingsAndControls::set_grid_resolution_range(int min_resolution, int max_resolution) {
    const int min_clamped = std::max(0, min_resolution);
    const int max_clamped = std::max(min_clamped, max_resolution);
    grid_resolution_min_ = min_clamped;
    grid_resolution_max_ = max_clamped;
    if (!grid_resolution_stepper_) {
        grid_resolution_stepper_ = std::make_unique<DMNumericStepper>("Tile Resolution (r)", min_clamped, max_clamped, min_clamped);
        grid_resolution_stepper_->set_on_change([this](int value) {
            if (on_grid_resolution_changed_) {
                on_grid_resolution_changed_(value);
            }
        });
    } else {
        grid_resolution_stepper_->set_range(min_clamped, max_clamped);
        const int current = grid_resolution_stepper_->value();
        const int clamped = std::clamp(current, min_clamped, max_clamped);
        grid_resolution_stepper_->set_value(clamped);
    }
    ensure_dev_settings_controls();
    if (overlay_grid_stepper_) {
        overlay_grid_stepper_->set_range(min_clamped, max_clamped);
        overlay_grid_stepper_->set_value(std::clamp(dev_mode_settings_.overlay_resolution, min_clamped, max_clamped));
    }
    layout_dirty_ = true;
}

void OtherSettingsAndControls::set_grid_resolution_value(int resolution) {
    if (!grid_resolution_stepper_) {
        return;
    }
    const int clamped = std::clamp(resolution, grid_resolution_min_, grid_resolution_max_);
    if (grid_resolution_stepper_->value() == clamped) {
        return;
    }
    grid_resolution_stepper_->set_value(clamped);
    layout_dirty_ = true;
}

int OtherSettingsAndControls::grid_resolution_value() const {
    if (!grid_resolution_stepper_) {
        return grid_resolution_min_;
    }
    return grid_resolution_stepper_->value();
}

void OtherSettingsAndControls::set_grid_resolution_change_callback(std::function<void(int)> cb) {
    on_grid_resolution_changed_ = std::move(cb);
}

OtherSettingsAndControls::FilterState& OtherSettingsAndControls::mutable_state() {
    if (!state_) {
        ensure_persistent_state_loaded();
        state_ = &persistent_state();
        has_saved_state_ = persistent_state_initialized_flag();
        filters_expanded_ = persistent_filters_expanded_flag();
        if (!has_saved_state_) {
            state_->map_assets = true;
            state_->current_room = true;
            state_->fog = true;
        }
    }
    return *state_;
}

const OtherSettingsAndControls::FilterState& OtherSettingsAndControls::state() const {
    return const_cast<OtherSettingsAndControls*>(this)->mutable_state();
}

void OtherSettingsAndControls::initialize() {
    entries_.clear();
    load_persisted_state();

    FilterState& state_ref = mutable_state();
    const bool use_saved_state = has_saved_state_;

    FilterEntry map_entry;
    map_entry.id = "map_assets";
    map_entry.kind = FilterKind::MapAssets;
    const bool map_assets_value = use_saved_state ? state_ref.map_assets : true;
    map_entry.checkbox = std::make_unique<DMCheckbox>("Map Assets", map_assets_value);
    if (!use_saved_state) {
        state_ref.map_assets = map_assets_value;
    }
    entries_.push_back(std::move(map_entry));

    FilterEntry room_entry;
    room_entry.id = "current_room";
    room_entry.kind = FilterKind::CurrentRoom;
    const bool current_room_value = use_saved_state ? state_ref.current_room : true;
    room_entry.checkbox = std::make_unique<DMCheckbox>("Current Room", current_room_value);
    if (!use_saved_state) {
        state_ref.current_room = current_room_value;
    }
    entries_.push_back(std::move(room_entry));

    FilterEntry fog_entry;
    fog_entry.id = "fog";
    fog_entry.kind = FilterKind::Fog;
    const bool fog_value = use_saved_state ? state_ref.fog : true;
    fog_entry.checkbox = std::make_unique<DMCheckbox>("Fog", fog_value);
    if (!use_saved_state) {
        state_ref.fog = fog_value;
    }
    entries_.push_back(std::move(fog_entry));

    static const std::vector<std::string> kSpawnMethods = {
        "Random",
        "Perimeter",
        "Edge",
        "Exact",
        "Exact Position",
        "Percent",
        "Center",
        "ChildRandom",
};

    std::unordered_set<std::string> known_methods;
    known_methods.reserve(kSpawnMethods.size());
    for (const std::string& method : kSpawnMethods) {
        const std::string canonical = canonicalize_method(method);
        FilterEntry entry;
        entry.id = canonical;
        entry.kind = FilterKind::SpawnMethod;
        bool checkbox_value = default_method_enabled(canonical);
        if (use_saved_state) {
            checkbox_value = load_method_filter_value(canonical, checkbox_value);
        }
        entry.checkbox = std::make_unique<DMCheckbox>(format_method_label(method), checkbox_value);
        state_ref.method_filters[canonical] = checkbox_value;
        known_methods.insert(canonical);
        entries_.push_back(std::move(entry));
    }

    const auto all_types = asset_types::all_as_strings();
    std::unordered_set<std::string> known_types;
    known_types.reserve(all_types.size());
    for (const std::string& type : all_types) {
        const std::string canonical = asset_types::canonicalize(type);
        FilterEntry entry;
        entry.id = canonical;
        entry.kind = FilterKind::Type;
        const bool default_enabled = default_type_enabled(canonical);
        bool checkbox_value = default_enabled;
        if (use_saved_state) {
            checkbox_value = load_type_filter_value(canonical, checkbox_value);
        }
        entry.checkbox = std::make_unique<DMCheckbox>(format_type_label(type), checkbox_value);
        state_ref.type_filters[canonical] = checkbox_value;
        known_types.insert(canonical);
        entries_.push_back(std::move(entry));
    }

    if (use_saved_state) {
        for (auto it = state_ref.type_filters.begin(); it != state_ref.type_filters.end();) {
            if (known_types.find(it->first) == known_types.end()) {
                it = state_ref.type_filters.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = state_ref.method_filters.begin(); it != state_ref.method_filters.end();) {
            if (known_methods.find(it->first) == known_methods.end()) {
                it = state_ref.method_filters.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!use_saved_state) {
        filters_expanded_ = false;
    }
    filter_toggle_button_ = std::make_unique<DMButton>(std::string(DMIcons::CollapseExpanded()), &DMStyles::HeaderButton(), std::max(DMButton::height(), kToggleButtonMinWidth), DMButton::height());
    hide_button_ = std::make_unique<DMButton>("^", &DMStyles::HeaderButton(), std::max(DMButton::height(), kHideButtonMinWidth), DMButton::height());
    update_filter_toggle_label();
    ensure_dev_settings_controls();
    sync_state_from_ui();
    layout_dirty_ = true;
    ensure_layout();
}

void OtherSettingsAndControls::set_state_changed_callback(StateChangedCallback cb) {
    on_state_changed_ = std::move(cb);
}

void OtherSettingsAndControls::set_enabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    layout_dirty_ = true;
}

void OtherSettingsAndControls::set_screen_dimensions(int width, int height) {
    if (screen_w_ == width && screen_h_ == height) {
        return;
    }
    screen_w_ = width;
    screen_h_ = height;
    layout_dirty_ = true;
}

void OtherSettingsAndControls::set_map_info(nlohmann::json* map_info) {
    map_info_json_ = map_info;
    rebuild_map_spawn_ids();
    notify_state_changed();
}

void OtherSettingsAndControls::set_current_room(Room* room) {
    current_room_ = room;
    rebuild_room_spawn_ids();
    notify_state_changed();
}

void OtherSettingsAndControls::set_header_title(const std::string& title) {
    header_title_ = title;
    layout_dirty_ = true;
}

void OtherSettingsAndControls::set_mode_buttons(std::vector<ModeButtonConfig> buttons) {
    mode_buttons_.clear();
    mode_buttons_.reserve(buttons.size());
    for (auto& cfg : buttons) {
        ModeButtonEntry entry;
        entry.config = std::move(cfg);
        entry.button = std::make_unique<DMButton>(entry.config.label, entry.config.active ? &DMStyles::AccentButton() : &DMStyles::HeaderButton(), 180, DMButton::height());
        mode_buttons_.push_back(std::move(entry));
    }
    layout_dirty_ = true;
}

void OtherSettingsAndControls::set_mode_changed_callback(std::function<void(const std::string&)> cb) {
    on_mode_selected_ = std::move(cb);
}

void OtherSettingsAndControls::set_active_mode(const std::string& id, bool trigger_callback) {
    bool changed = false;
    for (auto& entry : mode_buttons_) {
        const bool should_be_active = (entry.config.id == id);
        if (entry.config.active != should_be_active) {
            entry.config.active = should_be_active;
            if (entry.button) {
                entry.button->set_style(entry.config.active ? &DMStyles::AccentButton() : &DMStyles::HeaderButton());
            }
            changed = true;
        }
    }
    if (changed) {
        layout_dirty_ = true;
        if (trigger_callback && on_mode_selected_) {
            on_mode_selected_(id);
        }
    } else if (trigger_callback && on_mode_selected_) {
        on_mode_selected_(id);
    }
}

void OtherSettingsAndControls::set_filters_expanded(bool expanded) {
    if (filters_expanded_ == expanded) {
        return;
    }
    filters_expanded_ = expanded;
    update_filter_toggle_label();
    persist_filters_expanded();
    layout_dirty_ = true;
    if (filters_expanded_) {
        manual_hidden_lock_ = false;
        debounce_pending_ = false;
        slide_active_ = false;
        auto_hidden_ = false;
        layout_offset_y_ = 0;
        ensure_layout();
    } else {
        auto_hidden_ = true;
    }
    if (!filters_expanded_) {
        stats_.last_sample_ms = 0;
        last_cpu_sample_ms_ = 0;
        stats_.lines = {};
    }
}

void OtherSettingsAndControls::set_dev_mode_settings(const DevModeSettings& settings) {
    dev_mode_settings_ = settings;
    ensure_dev_settings_controls();
    if (overlay_grid_checkbox_) {
        overlay_grid_checkbox_->set_value(dev_mode_settings_.show_grid);
    }
    if (overlay_grid_stepper_) {
        const int clamped = std::clamp(dev_mode_settings_.overlay_resolution, grid_resolution_min_, grid_resolution_max_);
        overlay_grid_stepper_->set_value(clamped);
    }
    if (snap_to_grid_checkbox_) {
        snap_to_grid_checkbox_->set_value(dev_mode_settings_.snap_to_grid);
    }
    if (movement_debug_checkbox_) {
        movement_debug_checkbox_->set_value(dev_mode_settings_.movement_debug);
    }
    if (anchor_point_debug_checkbox_) {
        anchor_point_debug_checkbox_->set_value(dev_mode_settings_.anchor_point_debug);
    }
    if (depth_effects_checkbox_) {
        depth_effects_checkbox_->set_value(dev_mode_settings_.depth_effects);
    }
    layout_dirty_ = true;
}

void OtherSettingsAndControls::set_dev_mode_settings_callbacks(std::function<void(bool)> on_show_grid,
                                                               std::function<void(int)> on_overlay_resolution_change,
                                                               std::function<void(bool)> on_snap_to_grid,
                                                               std::function<void(bool)> on_movement_debug,
                                                               std::function<void(bool)> on_anchor_point_debug,
                                                               std::function<void(bool)> on_depth_effects) {
    on_show_grid_toggle_ = std::move(on_show_grid);
    on_overlay_resolution_change_ = std::move(on_overlay_resolution_change);
    on_snap_to_grid_toggle_ = std::move(on_snap_to_grid);
    on_movement_debug_toggle_ = std::move(on_movement_debug);
    on_anchor_point_debug_toggle_ = std::move(on_anchor_point_debug);
    on_depth_effects_toggle_ = std::move(on_depth_effects);
}

void OtherSettingsAndControls::set_show_grid_enabled(bool enabled) {
    dev_mode_settings_.show_grid = enabled;
    ensure_dev_settings_controls();
    if (overlay_grid_checkbox_) {
        overlay_grid_checkbox_->set_value(enabled);
    }
}

void OtherSettingsAndControls::set_overlay_resolution_value(int resolution) {
    const int clamped = std::clamp(resolution, grid_resolution_min_, grid_resolution_max_);
    dev_mode_settings_.overlay_resolution = clamped;
    ensure_dev_settings_controls();
    if (overlay_grid_stepper_) {
        overlay_grid_stepper_->set_value(clamped);
    }
}

void OtherSettingsAndControls::set_snap_to_grid_enabled(bool enabled) {
    dev_mode_settings_.snap_to_grid = enabled;
    ensure_dev_settings_controls();
    if (snap_to_grid_checkbox_) {
        snap_to_grid_checkbox_->set_value(enabled);
    }
}

void OtherSettingsAndControls::set_movement_debug_enabled(bool enabled) {
    dev_mode_settings_.movement_debug = enabled;
    ensure_dev_settings_controls();
    if (movement_debug_checkbox_) {
        movement_debug_checkbox_->set_value(enabled);
    }
}


void OtherSettingsAndControls::set_anchor_point_debug_enabled(bool enabled) {
    dev_mode_settings_.anchor_point_debug = enabled;
    ensure_dev_settings_controls();
    if (anchor_point_debug_checkbox_) {
        anchor_point_debug_checkbox_->set_value(enabled);
    }
}
void OtherSettingsAndControls::set_depth_effects_enabled(bool enabled) {
    dev_mode_settings_.depth_effects = enabled;
    ensure_dev_settings_controls();
    if (depth_effects_checkbox_) {
        depth_effects_checkbox_->set_value(enabled);
    }
}

void OtherSettingsAndControls::set_header_suppressed(bool suppressed) {
    if (header_suppressed_ == suppressed) {
        return;
    }
    header_suppressed_ = suppressed;
    if (header_suppressed_) {
        slide_active_ = false;
        debounce_pending_ = false;
    }
    layout_dirty_ = true;
}

void OtherSettingsAndControls::update(const Input& input) {
    if (!enabled_ || header_suppressed_ || screen_h_ <= 0) {
        return;
    }

    ensure_layout();

    const Uint64 now_ms = SDL_GetTicks();
    const float cursor_ratio = static_cast<float>(input.getY()) / static_cast<float>(std::max(1, screen_h_));
    const bool in_show_zone = cursor_ratio <= kHeaderShowZoneRatio;
    const bool below_unlock_zone = cursor_ratio > kHeaderUnlockZoneRatio;

    if (filters_expanded_) {
        manual_hidden_lock_ = false;
        debounce_pending_ = false;
        slide_active_ = false;
        auto_hidden_ = false;
        if (layout_offset_y_ != 0) {
            layout_offset_y_ = 0;
            layout_dirty_ = true;
            ensure_layout();
        }
        return;
    }

    if (manual_hidden_lock_) {
        if (below_unlock_zone) {
            manual_hidden_lock_ = false;
        } else {
            request_hidden_state(true, now_ms, true);
        }
    }

    if (!manual_hidden_lock_) {
        const bool should_hide = !in_show_zone;
        request_hidden_state(should_hide, now_ms, false);
    }

    update_slide(now_ms);
}

void OtherSettingsAndControls::refresh_layout() {
    layout_dirty_ = true;
    ensure_layout();
}

void OtherSettingsAndControls::ensure_layout() {
    if (!layout_dirty_) {
        return;
    }
    layout_dirty_ = false;
    rebuild_layout();
}

void OtherSettingsAndControls::rebuild_layout() {
    layout_bounds_ = SDL_Rect{0, 0, 0, 0};
    mode_bar_rect_ = SDL_Rect{0, 0, 0, 0};
    header_rect_ = SDL_Rect{0, 0, 0, 0};
    hide_button_rect_ = SDL_Rect{0, 0, 0, 0};
    settings_rect_ = SDL_Rect{0, 0, 0, 0};
    settings_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    filters_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    filters_rect_ = SDL_Rect{0, 0, 0, 0};

    clear_checkbox_rects();

    if (!enabled_ || screen_w_ <= 0) {
        return;
    }

    const int available_width = std::max(0, screen_w_);
    if (available_width <= 0) {
        return;
    }

    auto merge_bounds = [this](const SDL_Rect& rect) {
        if (rect.w <= 0 || rect.h <= 0) {
            return;
        }
        if (layout_bounds_.w <= 0 || layout_bounds_.h <= 0) {
            layout_bounds_ = rect;
            return;
        }
        int min_x = std::min(layout_bounds_.x, rect.x);
        int min_y = std::min(layout_bounds_.y, rect.y);
        int max_x = std::max(layout_bounds_.x + layout_bounds_.w, rect.x + rect.w);
        int max_y = std::max(layout_bounds_.y + layout_bounds_.h, rect.y + rect.h);
        layout_bounds_ = SDL_Rect{min_x, min_y, max_x - min_x, max_y - min_y};
};

    const int header_height = DMButton::height() + DMSpacing::item_gap() * 2;
    const int toggle_button_width = filter_toggle_button_
        ? std::max(filter_toggle_button_->preferred_width(), kToggleButtonMinWidth)
        : std::max(DMButton::height(), kToggleButtonMinWidth);
    header_rect_ = SDL_Rect{0, 0, available_width, header_height};

    if (hide_button_) {
        const int button_width = std::max(hide_button_->preferred_width(), kHideButtonMinWidth);
        const int button_height = DMButton::height();
        const int button_x = header_rect_.x + DMSpacing::item_gap();
        int button_y = header_rect_.y + (header_rect_.h - button_height) / 2;
        if (button_y < header_rect_.y) {
            button_y = header_rect_.y;
        }
        hide_button_->set_rect(SDL_Rect{button_x, button_y, button_width, button_height});
        hide_button_rect_ = hide_button_->rect();
    }

    if (filter_toggle_button_) {
        const int button_height = DMButton::height();
        int button_x = header_rect_.x + header_rect_.w - toggle_button_width - DMSpacing::item_gap();
        const int min_button_x = header_rect_.x + DMSpacing::item_gap();
        if (button_x < min_button_x) {
            button_x = min_button_x;
        }
        int button_y = header_rect_.y + (header_rect_.h - button_height) / 2;
        if (button_y < header_rect_.y) {
            button_y = header_rect_.y;
        }
        filter_toggle_button_->set_rect(SDL_Rect{button_x, button_y, toggle_button_width, button_height});
    }

    mode_bar_rect_ = header_rect_;
    if (hide_button_ && hide_button_rect_.w > 0) {
        const int right = mode_bar_rect_.x + mode_bar_rect_.w;
        const int left = hide_button_rect_.x + hide_button_rect_.w + DMSpacing::item_gap();
        mode_bar_rect_.x = std::min(right, left);
        mode_bar_rect_.w = std::max(0, right - mode_bar_rect_.x);
    }
    if (filter_toggle_button_) {
        const SDL_Rect& toggle_rect = filter_toggle_button_->rect();
        if (toggle_rect.w > 0) {
            int right_limit = std::max(mode_bar_rect_.x, toggle_rect.x - DMSpacing::item_gap());

            if (right_accessory_width_ > 0) {
                right_limit -= (right_accessory_width_ + DMSpacing::item_gap());
                right_limit = std::max(mode_bar_rect_.x, right_limit);
            }
            mode_bar_rect_.w = std::max(0, right_limit - mode_bar_rect_.x);
        }
    }

    merge_bounds(header_rect_);

    layout_mode_buttons();

    if (filters_expanded_) {
        int current_y = header_rect_.y + header_rect_.h;
        settings_rect_ = SDL_Rect{0, current_y, available_width, 0};
        layout_dev_settings();
        merge_bounds(settings_rect_);
        current_y = settings_rect_.y + settings_rect_.h;

        filters_rect_ = SDL_Rect{0, current_y, available_width, 0};
        layout_filter_checkboxes();
        layout_stats_panel();

        extra_panel_rect_ = SDL_Rect{0,0,0,0};
        if (extra_panel_height_ > 0) {
            const int top_gap = DMSpacing::item_gap();
            const int extra_y = filters_rect_.y + filters_rect_.h + top_gap;
            extra_panel_rect_ = SDL_Rect{ filters_rect_.x, extra_y, filters_rect_.w, extra_panel_height_ };

            filters_rect_.h += top_gap + extra_panel_height_;
        }
        merge_bounds(filters_rect_);
    }

    shift_all_by(layout_offset_y_);
}

int OtherSettingsAndControls::hidden_offset_y() const {
    const int layout_height = (layout_bounds_.h > 0) ? layout_bounds_.h : header_rect_.h;
    return -std::max(1, layout_height);
}

void OtherSettingsAndControls::begin_slide(bool hidden, Uint64 now_ms) {
    auto_hidden_ = hidden;
    const int target = hidden ? hidden_offset_y() : 0;
    if (layout_offset_y_ == target) {
        slide_active_ = false;
        return;
    }
    slide_active_ = true;
    slide_start_y_ = layout_offset_y_;
    slide_target_y_ = target;
    slide_started_ms_ = now_ms;
}

void OtherSettingsAndControls::update_slide(Uint64 now_ms) {
    if (!slide_active_) {
        return;
    }

    const Uint64 elapsed = now_ms - slide_started_ms_;
    if (elapsed >= kHeaderSlideDurationMs) {
        slide_active_ = false;
        if (layout_offset_y_ != slide_target_y_) {
            const int delta = slide_target_y_ - layout_offset_y_;
            layout_offset_y_ = slide_target_y_;
            shift_all_by(delta);
        }
        return;
    }

    const float t = static_cast<float>(elapsed) / static_cast<float>(kHeaderSlideDurationMs);
    const float eased = smoothstep(t);
    const float y = static_cast<float>(slide_start_y_) +
        (static_cast<float>(slide_target_y_ - slide_start_y_) * eased);
    const int next_y = static_cast<int>(std::lround(y));
    if (next_y != layout_offset_y_) {
        const int delta = next_y - layout_offset_y_;
        layout_offset_y_ = next_y;
        shift_all_by(delta);
    }
}

void OtherSettingsAndControls::request_hidden_state(bool hidden, Uint64 now_ms, bool bypass_debounce) {
    if (hidden == auto_hidden_) {
        debounce_pending_ = false;
        return;
    }

    if (bypass_debounce) {
        debounce_pending_ = false;
        begin_slide(hidden, now_ms);
        return;
    }

    if (!debounce_pending_ || debounce_hidden_target_ != hidden) {
        debounce_pending_ = true;
        debounce_hidden_target_ = hidden;
        debounce_started_ms_ = now_ms;
        return;
    }

    if (now_ms - debounce_started_ms_ >= kHeaderZoneDebounceMs) {
        debounce_pending_ = false;
        begin_slide(hidden, now_ms);
    }
}

void OtherSettingsAndControls::render(SDL_Renderer* renderer) const {
    if (!enabled_ || !renderer || header_suppressed_) {
        return;
    }
    const_cast<OtherSettingsAndControls*>(this)->ensure_layout();
    if (layout_bounds_.w <= 0 || layout_bounds_.h <= 0) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const bool expanded = filters_expanded_;
    const SDL_Color base_panel_bg = DMStyles::PanelBG();
    const SDL_Color panel_bg = expanded ? reduced_alpha_color(base_panel_bg) : base_panel_bg;
    const SDL_Color highlight = DMStyles::HighlightColor();
    const SDL_Color shadow = DMStyles::ShadowColor();
    dm_draw::DrawBeveledRect( renderer, layout_bounds_, DMStyles::CornerRadius(), DMStyles::BevelDepth(), panel_bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

    const SDL_Color border = DMStyles::Border();
    dm_draw::DrawRoundedOutline( renderer, layout_bounds_, DMStyles::CornerRadius(), kPanelOutlineThickness, border);

    const SDL_Color header_bg = DMStyles::PanelHeader();
    if (header_rect_.w > 0 && header_rect_.h > 0) {
        SDL_SetRenderDrawColor(renderer, header_bg.r, header_bg.g, header_bg.b, 240);
        sdl_render::FillRect(renderer, &header_rect_);
    }

    if (hide_button_) {
        hide_button_->render(renderer);
    }

    if (filter_toggle_button_) {
        filter_toggle_button_->render(renderer);
    }

    for (const auto& entry : mode_buttons_) {
        if (entry.button) {
            entry.button->render(renderer);
        }
    }

    if (!header_title_display_.empty() && header_title_rect_.w > 0 && header_title_rect_.h > 0) {
        const DMLabelStyle& style = DMStyles::Label();
        SDL_Point text_size = MeasureLabelText(style, header_title_display_);
        int text_y = header_title_rect_.y + (header_title_rect_.h - text_size.y) / 2;
        if (text_y < header_title_rect_.y) {
            text_y = header_title_rect_.y;
        }
        DrawLabelText(renderer, header_title_display_, header_title_rect_.x, text_y, style);
    }

    if (!filters_expanded_) {
        return;
    }

    const SDL_Color content_bg = expanded ? panel_bg : base_panel_bg;
    const Uint8 content_alpha = expanded ? content_bg.a : 220;
    if (settings_rect_.w > 0 && settings_rect_.h > 0) {
        SDL_SetRenderDrawColor(renderer, content_bg.r, content_bg.g, content_bg.b, content_alpha);
        sdl_render::FillRect(renderer, &settings_rect_);
    }

    if (settings_heading_rect_.w > 0 && settings_heading_rect_.h > 0) {
        DrawLabelText(renderer, kDevSettingsTitle, settings_heading_rect_.x, settings_heading_rect_.y, DMStyles::Label());
    }

    if (grid_section_label_rect_.w > 0 && grid_section_label_rect_.h > 0) {
        DrawLabelText(renderer, kGridSectionTitle, grid_section_label_rect_.x, grid_section_label_rect_.y, DMStyles::Label());
    }
    if (debug_section_label_rect_.w > 0 && debug_section_label_rect_.h > 0) {
        DrawLabelText(renderer, kDebugSectionTitle, debug_section_label_rect_.x, debug_section_label_rect_.y, DMStyles::Label());
    }
    if (overlay_section_label_rect_.w > 0 && overlay_section_label_rect_.h > 0) {
        DrawLabelText(renderer, kOverlaySectionTitle, overlay_section_label_rect_.x, overlay_section_label_rect_.y, DMStyles::Label());
    }

    if (overlay_grid_checkbox_) {
        overlay_grid_checkbox_->render(renderer);
    }
    if (snap_to_grid_checkbox_) {
        snap_to_grid_checkbox_->render(renderer);
    }
    if (movement_debug_checkbox_) {
        movement_debug_checkbox_->render(renderer);
    }
    if (anchor_point_debug_checkbox_) {
        anchor_point_debug_checkbox_->render(renderer);
    }
    if (depth_effects_checkbox_) {
        depth_effects_checkbox_->render(renderer);
    }
    if (overlay_grid_stepper_) {
        overlay_grid_stepper_->render(renderer);
    }

    if (!filters_expanded_) {
        return;
    }

    if (filters_rect_.w > 0 && filters_rect_.h > 0) {
        SDL_SetRenderDrawColor(renderer, content_bg.r, content_bg.g, content_bg.b, content_alpha);
        sdl_render::FillRect(renderer, &filters_rect_);
    }

    if (filters_heading_rect_.w > 0 && filters_heading_rect_.h > 0) {
        DrawLabelText(renderer, kFiltersTitle, filters_heading_rect_.x, filters_heading_rect_.y, DMStyles::Label());
    }

    if (primary_filters_heading_rect_.w > 0 && primary_filters_heading_rect_.h > 0) {
        DrawLabelText(renderer, kPrimaryFiltersTitle, primary_filters_heading_rect_.x, primary_filters_heading_rect_.y, DMStyles::Label());
    }
    if (advanced_filters_heading_rect_.w > 0 && advanced_filters_heading_rect_.h > 0) {
        DrawLabelText(renderer, kAdvancedFiltersTitle, advanced_filters_heading_rect_.x, advanced_filters_heading_rect_.y, DMStyles::Label());
    }
    if (grid_resolution_label_rect_.w > 0 && grid_resolution_label_rect_.h > 0) {
        DrawLabelText(renderer, kGridResolutionTitle, grid_resolution_label_rect_.x, grid_resolution_label_rect_.y, DMStyles::Label());
    }

    for (const auto& entry : entries_) {
        if (entry.checkbox) {
            entry.checkbox->render(renderer);
        }
    }

    if (grid_resolution_stepper_) {
        grid_resolution_stepper_->render(renderer);
    }

    render_stats(renderer);

    if (extra_panel_rect_.w > 0 && extra_panel_rect_.h > 0 && extra_renderer_) {
        extra_renderer_(renderer, extra_panel_rect_);
    }
}

bool OtherSettingsAndControls::handle_event(const SDL_Event& event) {
    if (!enabled_ || header_suppressed_) {
        return false;
    }
    ensure_layout();
    bool used = false;
    if (hide_button_ && hide_button_->handle_event(event)) {
        used = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            manual_hidden_lock_ = true;
            request_hidden_state(true, SDL_GetTicks(), true);
        }
    }

    auto handle_button = [&](ModeButtonEntry& entry) {
        if (!entry.button) {
            return;
        }
        if (entry.button->handle_event(event)) {
            used = true;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
                set_active_mode(entry.config.id, true);
            }
        }
};

    for (auto& entry : mode_buttons_) {
        handle_button(entry);
    }

    if (filter_toggle_button_ && filter_toggle_button_->handle_event(event)) {
        used = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            set_filters_expanded(!filters_expanded_);
            ensure_layout();
        }
    }

    if (!filters_expanded_) {
        return used;
    }

    auto handle_setting_checkbox = [&](DMCheckbox* checkbox, std::function<void(bool)> cb, bool* backing_value) {
        if (!checkbox) {
            return;
        }
        const bool before = checkbox->value();
        if (checkbox->handle_event(event)) {
            used = true;
            const bool after = checkbox->value();
            if (backing_value) {
                *backing_value = after;
            }
            if (cb && before != after) {
                cb(after);
            }
        }
    };

    auto handle_overlay_stepper = [&](DMNumericStepper* stepper, std::function<void(int)> cb, int* backing_value) {
        if (!stepper) {
            return;
        }
        const int before = stepper->value();
        if (stepper->handle_event(event)) {
            used = true;
            const int after = stepper->value();
            if (backing_value) {
                *backing_value = after;
            }
            if (cb && before != after) {
                cb(after);
            }
        }
    };

    handle_setting_checkbox(overlay_grid_checkbox_.get(), on_show_grid_toggle_, &dev_mode_settings_.show_grid);
    handle_setting_checkbox(snap_to_grid_checkbox_.get(), on_snap_to_grid_toggle_, &dev_mode_settings_.snap_to_grid);
    handle_setting_checkbox(movement_debug_checkbox_.get(), on_movement_debug_toggle_, &dev_mode_settings_.movement_debug);
    handle_setting_checkbox(anchor_point_debug_checkbox_.get(), on_anchor_point_debug_toggle_, &dev_mode_settings_.anchor_point_debug);
    handle_setting_checkbox(depth_effects_checkbox_.get(), on_depth_effects_toggle_, &dev_mode_settings_.depth_effects);
    handle_overlay_stepper(overlay_grid_stepper_.get(), on_overlay_resolution_change_, &dev_mode_settings_.overlay_resolution);

    bool checkbox_used = false;
    for (auto& entry : entries_) {
        if (!entry.checkbox) {
            continue;
        }
        if (entry.checkbox->handle_event(event)) {
            checkbox_used = true;
        }
    }
    if (checkbox_used) {
        used = true;
        sync_state_from_ui();
        notify_state_changed();
    }

    if (grid_resolution_stepper_ && grid_resolution_stepper_->handle_event(event)) {
        used = true;
    }

    if (extra_panel_rect_.w > 0 && extra_panel_rect_.h > 0 && extra_event_handler_) {

        if (extra_event_handler_(event, extra_panel_rect_)) {
            used = true;
        }
    }
    return used;
}

bool OtherSettingsAndControls::contains_point(int x, int y) const {
    if (!enabled_ || header_suppressed_) {
        return false;
    }
    const_cast<OtherSettingsAndControls*>(this)->ensure_layout();
    SDL_Point p{x, y};
    if (layout_bounds_.w > 0 && layout_bounds_.h > 0 && SDL_PointInRect(&p, &layout_bounds_)) {
        return true;
    }
    return false;
}

void OtherSettingsAndControls::reset() {
    for (auto& entry : entries_) {
        if (!entry.checkbox) {
            continue;
        }
        switch (entry.kind) {
            case FilterKind::MapAssets:
                entry.checkbox->set_value(true);
                break;
            case FilterKind::CurrentRoom:
                entry.checkbox->set_value(true);
                break;
            case FilterKind::Fog:
                entry.checkbox->set_value(true);
                break;
            case FilterKind::Type:
                entry.checkbox->set_value(default_type_enabled(entry.id));
                break;
            case FilterKind::SpawnMethod:
                entry.checkbox->set_value(default_method_enabled(entry.id));
                break;
        }
    }
    FilterState& state_ref = mutable_state();
    state_ref.map_assets = true;
    state_ref.current_room = true;
    state_ref.fog = true;
    for (auto& kv : state_ref.type_filters) {
        kv.second = default_type_enabled(kv.first);
    }
    for (auto& kv : state_ref.method_filters) {
        kv.second = default_method_enabled(kv.first);
    }
    sync_state_from_ui();
    notify_state_changed();
}

bool OtherSettingsAndControls::default_type_enabled(const std::string& type) const {
    return true;
}

bool OtherSettingsAndControls::default_method_enabled(const std::string& method) const {
    (void)method;
    return true;
}

bool OtherSettingsAndControls::fog_visible() const {
    if (!enabled_) {
        return true;
    }
    return state().fog;
}

bool OtherSettingsAndControls::passes(const Asset& asset) const {
    if (!enabled_) {
        return true;
    }
    if (!asset.info) {
        return true;
    }
    const std::string& type = asset.filter_type_tag();
    if (!type.empty() && !type_filter_enabled(type)) {
        return false;
    }
    const std::string& method = asset.filter_method_tag();
    if (!method.empty() && !method_filter_enabled(method)) {
        return false;
    }
    const bool is_map_asset = !asset.spawn_id.empty() && map_spawn_ids_.find(asset.spawn_id) != map_spawn_ids_.end();
    const FilterState& state_ref = state();
    if (is_map_asset && !state_ref.map_assets) {
        return false;
    }
    const bool is_room_asset = !asset.spawn_id.empty() && room_spawn_ids_.find(asset.spawn_id) != room_spawn_ids_.end();
    if (is_room_asset && !state_ref.current_room) {
        return false;
    }
    return true;
}

bool OtherSettingsAndControls::is_type_filter_enabled(const std::string& type) const {
    return type_filter_enabled(type);
}

void OtherSettingsAndControls::rebuild_map_spawn_ids() {
    map_spawn_ids_.clear();
    if (!map_info_json_) {
        return;
    }
    try {
        auto it = map_info_json_->find("map_assets_data");
        if (it != map_info_json_->end()) {
            collect_spawn_ids(*it, map_spawn_ids_);
        }
    } catch (...) {
    }
}

void OtherSettingsAndControls::rebuild_room_spawn_ids() {
    room_spawn_ids_.clear();
    if (!current_room_) {
        return;
    }
    try {
        nlohmann::json& data = current_room_->assets_data();
        collect_spawn_ids(data, room_spawn_ids_);
    } catch (...) {
    }
}

void OtherSettingsAndControls::sync_state_from_ui() {
    FilterState& state_ref = mutable_state();
    for (auto& entry : entries_) {
        if (!entry.checkbox) {
            continue;
        }
        const bool value = entry.checkbox->value();
        switch (entry.kind) {
        case FilterKind::MapAssets:
            state_ref.map_assets = value;
            break;
        case FilterKind::CurrentRoom:
            state_ref.current_room = value;
            break;
        case FilterKind::Fog:
            state_ref.fog = value;
            break;
        case FilterKind::Type:
            state_ref.type_filters[entry.id] = value;
            break;
        case FilterKind::SpawnMethod:
            state_ref.method_filters[entry.id] = value;
            break;
        }
    }
    persist_state();
}

void OtherSettingsAndControls::notify_state_changed() {
    ++state_version_;
    if (state_version_ == 0) {
        ++state_version_;
    }
    if (on_state_changed_) {
        on_state_changed_();
    }
}

void OtherSettingsAndControls::update_filter_toggle_label() {
    if (!filter_toggle_button_) {
        return;
    }
    std::string label = "Dev Mode Settings ";
    label += filters_expanded_ ? std::string(DMIcons::CollapseExpanded()) : std::string(DMIcons::CollapseCollapsed());
    filter_toggle_button_->set_text(label);
}

void OtherSettingsAndControls::clear_checkbox_rects() {
    for (auto& entry : entries_) {
        if (entry.checkbox) {
            entry.checkbox->set_rect(SDL_Rect{0, 0, 0, 0});
        }
    }
    if (grid_resolution_stepper_) {
        grid_resolution_stepper_->set_rect(SDL_Rect{0, 0, 0, 0});
    }
    if (overlay_grid_checkbox_) {
        overlay_grid_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
    }
    if (overlay_grid_stepper_) {
        overlay_grid_stepper_->set_rect(SDL_Rect{0, 0, 0, 0});
    }
    if (snap_to_grid_checkbox_) {
        snap_to_grid_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
    }
    if (movement_debug_checkbox_) {
        movement_debug_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
    }
    if (anchor_point_debug_checkbox_) {
        anchor_point_debug_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
    }
    if (depth_effects_checkbox_) {
        depth_effects_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
    }
    grid_section_label_rect_ = SDL_Rect{0, 0, 0, 0};
    debug_section_label_rect_ = SDL_Rect{0, 0, 0, 0};
    overlay_section_label_rect_ = SDL_Rect{0, 0, 0, 0};
    primary_filters_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    advanced_filters_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    grid_resolution_label_rect_ = SDL_Rect{0, 0, 0, 0};
    stats_rect_ = SDL_Rect{0, 0, 0, 0};
    stats_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    for (auto& rect : stats_line_rects_) {
        rect = SDL_Rect{0, 0, 0, 0};
    }
}

void OtherSettingsAndControls::layout_mode_buttons() {
    header_title_rect_ = SDL_Rect{0, 0, 0, 0};
    header_title_display_.clear();

    const int count = static_cast<int>(mode_buttons_.size());
    for (auto& entry : mode_buttons_) {
        if (entry.button) {
            entry.button->set_style(entry.config.active ? &DMStyles::AccentButton() : &DMStyles::HeaderButton());
        }
    }

    if (mode_bar_rect_.w <= 0 || mode_bar_rect_.h <= 0) {
        for (auto& entry : mode_buttons_) {
            if (entry.button) {
                entry.button->set_rect(SDL_Rect{0, 0, 0, 0});
            }
        }
        return;
    }

    const int padding = DMSpacing::item_gap();
    const int inner_gap = DMSpacing::small_gap();
    const int text_padding = DMSpacing::small_gap();
    const int left = mode_bar_rect_.x + padding;
    const int right = mode_bar_rect_.x + mode_bar_rect_.w - padding;
    if (right <= left) {
        for (auto& entry : mode_buttons_) {
            if (entry.button) {
                entry.button->set_rect(SDL_Rect{0, 0, 0, 0});
            }
        }
        return;
    }

    if (count == 0) {
        header_title_rect_ = SDL_Rect{left, mode_bar_rect_.y, right - left, mode_bar_rect_.h};
        if (header_title_rect_.w > 0 && header_title_rect_.h > 0 && !header_title_.empty()) {
            const DMLabelStyle& style = DMStyles::Label();
            const int available_width = std::max(0, header_title_rect_.w - text_padding * 2);
            header_title_display_ = ellipsize_text(header_title_, available_width, style);
            header_title_rect_.x += text_padding;
            header_title_rect_.w = std::max(0, header_title_rect_.w - text_padding * 2);
        }
        return;
    }

    const int available_width = right - left;
    if (available_width <= 0) {
        for (auto& entry : mode_buttons_) {
            if (entry.button) {
                entry.button->set_rect(SDL_Rect{0, 0, 0, 0});
            }
        }
        return;
    }

    int base_segment = available_width / count;
    int remainder = available_width % count;

    int y = mode_bar_rect_.y + (mode_bar_rect_.h - DMButton::height()) / 2;
    if (y < mode_bar_rect_.y) {
        y = mode_bar_rect_.y;
    }

    int current_x = left;
    for (int i = 0; i < count; ++i) {
        auto& entry = mode_buttons_[i];
        if (!entry.button) {
            continue;
        }

        int segment = base_segment;
        if (remainder > 0) {
            ++segment;
            --remainder;
        }

        if (segment <= 0) {
            entry.button->set_rect(SDL_Rect{0, 0, 0, 0});
            continue;
        }

        int button_x = current_x + inner_gap;
        int button_width = segment - inner_gap * 2;
        if (button_width <= 0) {
            button_x = current_x;
            button_width = segment;
        }

        entry.button->set_rect(SDL_Rect{button_x, y, button_width, DMButton::height()});
        current_x += segment;
    }
}

void OtherSettingsAndControls::layout_dev_settings() {
    settings_rect_.h = 0;
    settings_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    grid_section_label_rect_ = SDL_Rect{0, 0, 0, 0};
    debug_section_label_rect_ = SDL_Rect{0, 0, 0, 0};
    overlay_section_label_rect_ = SDL_Rect{0, 0, 0, 0};
    ensure_dev_settings_controls();
    if (settings_rect_.w <= 0 || settings_rect_.h < 0) {
        return;
    }

    const int margin_x = DMSpacing::item_gap();
    const int margin_y = DMSpacing::item_gap();
    const int row_gap = DMSpacing::item_gap();
    const int section_gap = DMSpacing::section_gap();
    const int label_gap = DMSpacing::label_gap();
    const int content_width = std::max(0, settings_rect_.w - margin_x * 2);
    if (content_width <= 0) {
        return;
    }

    const int max_content_width = std::min(content_width, 960);
    const int content_left = settings_rect_.x + margin_x + (content_width - max_content_width) / 2;

    int y = settings_rect_.y + margin_y;
    const DMLabelStyle& heading_style = DMStyles::Label();
    settings_heading_rect_ = SDL_Rect{content_left, y, max_content_width, heading_style.font_size};
    y += settings_heading_rect_.h + label_gap;

    const int control_height = DMCheckbox::height();
    const bool wide_mode = max_content_width >= 520;
    const int column_gap = wide_mode ? DMSpacing::item_gap() * 2 : 0;
    const int column_width = wide_mode ? std::max(0, (max_content_width - column_gap) / 2) : max_content_width;
    const int left_column = content_left;
    const int right_column = content_left + column_width + (wide_mode ? column_gap : 0);

    grid_section_label_rect_ = SDL_Rect{content_left, y, max_content_width, heading_style.font_size};
    y += grid_section_label_rect_.h + row_gap;
    bool grid_row_started = false;
    if (overlay_grid_checkbox_) {
        overlay_grid_checkbox_->set_rect(SDL_Rect{left_column, y, column_width, control_height});
        grid_row_started = true;
    }
    const bool grid_two_column = wide_mode && overlay_grid_checkbox_ && snap_to_grid_checkbox_;
    if (snap_to_grid_checkbox_) {
        if (grid_two_column) {
            snap_to_grid_checkbox_->set_rect(SDL_Rect{right_column, y, column_width, control_height});
            y += control_height + row_gap;
            grid_row_started = false;
        } else {
            if (grid_row_started) {
                y += control_height;
            }
            snap_to_grid_checkbox_->set_rect(SDL_Rect{left_column, y, column_width, control_height});
            y += control_height + row_gap;
            grid_row_started = false;
        }
    } else if (grid_row_started) {
        y += control_height + row_gap;
        grid_row_started = false;
    }
    y += section_gap;

    debug_section_label_rect_ = SDL_Rect{content_left, y, max_content_width, heading_style.font_size};
    y += debug_section_label_rect_.h + row_gap;
    if (movement_debug_checkbox_) {
        movement_debug_checkbox_->set_rect(SDL_Rect{left_column, y, max_content_width, control_height});
        y += control_height + row_gap;
    }
    if (anchor_point_debug_checkbox_) {
        anchor_point_debug_checkbox_->set_rect(SDL_Rect{left_column, y, max_content_width, control_height});
        y += control_height + row_gap;
    }
    if (depth_effects_checkbox_) {
        depth_effects_checkbox_->set_rect(SDL_Rect{left_column, y, max_content_width, control_height});
        y += control_height + row_gap;
    }
    y += section_gap;

    overlay_section_label_rect_ = SDL_Rect{content_left, y, max_content_width, heading_style.font_size};
    y += overlay_section_label_rect_.h + row_gap;
    if (overlay_grid_stepper_) {
        const int stepper_width = max_content_width;
        const int stepper_height = overlay_grid_stepper_->preferred_height(stepper_width);
        overlay_grid_stepper_->set_rect(SDL_Rect{content_left, y, stepper_width, stepper_height});
        y += stepper_height;
    }
    y += margin_y;
    settings_rect_.h = std::max(0, y - settings_rect_.y);
}

void OtherSettingsAndControls::layout_filter_checkboxes() {
    filters_rect_.h = 0;
    if (!filters_expanded_ || filters_rect_.w <= 0) {
        return;
    }

    for (auto& entry : entries_) {
        if (entry.checkbox) {
            entry.checkbox->set_rect(SDL_Rect{0, 0, 0, 0});
        }
    }
    primary_filters_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    advanced_filters_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    grid_resolution_label_rect_ = SDL_Rect{0, 0, 0, 0};
    if (grid_resolution_stepper_) {
        grid_resolution_stepper_->set_rect(SDL_Rect{0, 0, 0, 0});
    }

    const int margin_x = DMSpacing::item_gap();
    const int margin_y = DMSpacing::item_gap();
    const int row_gap = DMSpacing::small_gap();
    const int section_gap = DMSpacing::section_gap();
    const int checkbox_width = 180;
    const int checkbox_height = DMCheckbox::height();
    const int available_width = std::max(0, filters_rect_.w - margin_x * 2);
    if (available_width <= 0) {
        return;
    }

    int y = filters_rect_.y + margin_y;
    filters_heading_rect_ = SDL_Rect{ filters_rect_.x + margin_x,
                                      y,
                                      std::max(0, filters_rect_.w - margin_x * 2),
                                      DMStyles::Label().font_size };
    y += filters_heading_rect_.h + kSectionHeaderSpacing;

    std::vector<FilterEntry*> primary_entries;
    std::vector<FilterEntry*> advanced_entries;
    primary_entries.reserve(entries_.size());
    advanced_entries.reserve(entries_.size());
    for (auto& entry : entries_) {
        if (!entry.checkbox) {
            continue;
        }
        switch (entry.kind) {
        case FilterKind::MapAssets:
        case FilterKind::CurrentRoom:
        case FilterKind::Fog:
            primary_entries.push_back(&entry);
            break;
        default:
            advanced_entries.push_back(&entry);
            break;
        }
    }

    auto build_rows_for = [&](const std::vector<FilterEntry*>& source) {
        std::vector<std::vector<FilterEntry*>> section_rows(1);
        for (FilterEntry* entry : source) {
            if (!entry || !entry->checkbox) {
                continue;
            }
            auto& current_row = section_rows.back();
            int current_width = 0;
            if (!current_row.empty()) {
                current_width = static_cast<int>(current_row.size()) * checkbox_width + static_cast<int>(current_row.size() - 1) * margin_x;
            }
            int width_with_new = current_width + checkbox_width;
            if (!current_row.empty()) {
                width_with_new += margin_x;
            }
            if (!current_row.empty() && width_with_new > available_width) {
                section_rows.emplace_back();
            }
            section_rows.back().push_back(entry);
        }
        if (!section_rows.empty() && section_rows.back().empty()) {
            section_rows.pop_back();
        }
        return section_rows;
};

    const auto primary_rows = build_rows_for(primary_entries);
    const auto advanced_rows = build_rows_for(advanced_entries);
    auto rows_have_content = [](const std::vector<std::vector<FilterEntry*>>& rows) {
        for (const auto& row : rows) {
            if (!row.empty()) {
                return true;
            }
        }
        return false;
};

    const bool has_primary = rows_have_content(primary_rows);
    const bool has_advanced = rows_have_content(advanced_rows);
    const bool has_grid_stepper = static_cast<bool>(grid_resolution_stepper_);
    if (!has_primary && !has_advanced) {
        return;
    }

    // `y` already points past the heading
    const int left_limit = filters_rect_.x + margin_x;
    const int right_limit = filters_rect_.x + filters_rect_.w - margin_x;
    const DMLabelStyle& subheading_style = DMStyles::Label();

    auto layout_rows = [&](const std::vector<std::vector<FilterEntry*>>& rows) {
        for (size_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
            const auto& row = rows[row_idx];
            if (row.empty()) {
                continue;
            }
            const int row_width = static_cast<int>(row.size()) * checkbox_width + static_cast<int>(row.size() - 1) * margin_x;
            int x = filters_rect_.x + (filters_rect_.w - row_width) / 2;
            if (row_width > (right_limit - left_limit)) {
                x = left_limit;
            } else {
                if (x < left_limit) x = left_limit;
                if (x + row_width > right_limit) {
                    x = right_limit - row_width;
                }
            }

            for (FilterEntry* entry : row) {
                if (!entry || !entry->checkbox) {
                    continue;
                }
                SDL_Rect rect{x, y, checkbox_width, checkbox_height};
                entry->checkbox->set_rect(rect);
                x += checkbox_width + margin_x;
            }

            y += checkbox_height;

            bool more_rows_pending = false;
            for (size_t next_row = row_idx + 1; next_row < rows.size(); ++next_row) {
                if (!rows[next_row].empty()) {
                    more_rows_pending = true;
                    break;
                }
            }
            if (more_rows_pending) {
                y += row_gap;
            }
        }
};

    bool section_emitted = false;
    if (has_primary) {
        primary_filters_heading_rect_ = SDL_Rect{filters_rect_.x + margin_x, y, available_width, subheading_style.font_size};
        y += primary_filters_heading_rect_.h + row_gap / 2;
        layout_rows(primary_rows);
        section_emitted = true;
        if (has_advanced || has_grid_stepper) {
            y += section_gap;
        }
    }

    if (has_advanced) {
        advanced_filters_heading_rect_ = SDL_Rect{filters_rect_.x + margin_x, y, available_width, subheading_style.font_size};
        y += advanced_filters_heading_rect_.h + row_gap / 2;
        layout_rows(advanced_rows);
        section_emitted = true;
        if (has_grid_stepper) {
            y += section_gap;
        }
    }

    if (grid_resolution_stepper_) {
        grid_resolution_label_rect_ = SDL_Rect{filters_rect_.x + margin_x, y, available_width, subheading_style.font_size};
        y += grid_resolution_label_rect_.h + row_gap / 2;
        const int stepper_width = std::max(0, filters_rect_.w - margin_x * 2);
        if (stepper_width > 0) {
            const int stepper_height = grid_resolution_stepper_->preferred_height(stepper_width);
            SDL_Rect rect{filters_rect_.x + margin_x, y, stepper_width, stepper_height};
            grid_resolution_stepper_->set_rect(rect);
            y += stepper_height;
        } else {
            grid_resolution_stepper_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        section_emitted = true;
    }

    y += margin_y;
    filters_rect_.h = y - filters_rect_.y;
}

void OtherSettingsAndControls::shift_all_by(int dy) {
    if (dy == 0) {
        return;
    }

    auto shift_rect = [dy](SDL_Rect& rect) {
        if (rect.w <= 0 && rect.h <= 0) {
            return;
        }
        rect.y += dy;
    };
    auto shift_button = [dy](DMButton* button) {
        if (!button) {
            return;
        }
        SDL_Rect rect = button->rect();
        if (rect.w <= 0 && rect.h <= 0) {
            return;
        }
        rect.y += dy;
        button->set_rect(rect);
    };
    auto shift_checkbox = [dy](DMCheckbox* checkbox) {
        if (!checkbox) {
            return;
        }
        SDL_Rect rect = checkbox->rect();
        if (rect.w <= 0 && rect.h <= 0) {
            return;
        }
        rect.y += dy;
        checkbox->set_rect(rect);
    };
    auto shift_stepper = [dy](DMNumericStepper* stepper) {
        if (!stepper) {
            return;
        }
        SDL_Rect rect = stepper->rect();
        if (rect.w <= 0 && rect.h <= 0) {
            return;
        }
        rect.y += dy;
        stepper->set_rect(rect);
    };

    shift_rect(layout_bounds_);
    shift_rect(mode_bar_rect_);
    shift_rect(header_rect_);
    shift_rect(hide_button_rect_);
    shift_rect(settings_rect_);
    shift_rect(settings_heading_rect_);
    shift_rect(grid_section_label_rect_);
    shift_rect(debug_section_label_rect_);
    shift_rect(overlay_section_label_rect_);
    shift_rect(filters_heading_rect_);
    shift_rect(primary_filters_heading_rect_);
    shift_rect(advanced_filters_heading_rect_);
    shift_rect(grid_resolution_label_rect_);
    shift_rect(filters_rect_);
    shift_rect(header_title_rect_);
    shift_rect(stats_rect_);
    shift_rect(stats_heading_rect_);
    shift_rect(extra_panel_rect_);
    for (SDL_Rect& rect : stats_line_rects_) {
        shift_rect(rect);
    }

    shift_button(filter_toggle_button_.get());
    shift_button(hide_button_.get());
    for (auto& entry : mode_buttons_) {
        shift_button(entry.button.get());
    }
    for (auto& entry : entries_) {
        shift_checkbox(entry.checkbox.get());
    }
    shift_checkbox(overlay_grid_checkbox_.get());
    shift_checkbox(snap_to_grid_checkbox_.get());
    shift_checkbox(movement_debug_checkbox_.get());
    shift_checkbox(anchor_point_debug_checkbox_.get());
    shift_checkbox(depth_effects_checkbox_.get());
    shift_stepper(overlay_grid_stepper_.get());
    shift_stepper(grid_resolution_stepper_.get());
}

void OtherSettingsAndControls::layout_stats_panel() {
    stats_rect_ = SDL_Rect{0, 0, 0, 0};
    stats_heading_rect_ = SDL_Rect{0, 0, 0, 0};
    for (auto& rect : stats_line_rects_) {
        rect = SDL_Rect{0, 0, 0, 0};
    }
    if (!filters_expanded_ || filters_rect_.w <= 0) {
        return;
    }

    const int margin_x = DMSpacing::item_gap();
    const int margin_y = DMSpacing::item_gap();
    const int row_gap = DMSpacing::small_gap();
    const int content_width = std::max(0, filters_rect_.w - margin_x * 2);
    if (content_width <= 0) {
        return;
    }

    int y = filters_rect_.y + filters_rect_.h + DMSpacing::section_gap();
    stats_rect_ = SDL_Rect{filters_rect_.x, y, filters_rect_.w, 0};
    y += margin_y;

    const DMLabelStyle& heading_style = DMStyles::Label();
    stats_heading_rect_ = SDL_Rect{stats_rect_.x + margin_x, y, content_width, heading_style.font_size};
    y += stats_heading_rect_.h + row_gap;

    const int line_height = heading_style.font_size;
    for (auto& rect : stats_line_rects_) {
        rect = SDL_Rect{stats_rect_.x + margin_x, y, content_width, line_height};
        y += line_height + row_gap;
    }

    y += margin_y;
    stats_rect_.h = y - stats_rect_.y;
    filters_rect_.h = (stats_rect_.y + stats_rect_.h) - filters_rect_.y;
}

void OtherSettingsAndControls::maybe_refresh_stats(SDL_Renderer* renderer) {
    if (!filters_expanded_) {
        return;
    }
    const Uint64 now = SDL_GetTicks();
    const Uint64 interval = std::max<Uint64>(stats_refresh_ms_, kStatsRefreshMs);
    if (stats_.last_sample_ms != 0 && now - stats_.last_sample_ms < interval) {
        return;
    }
    stats_.last_sample_ms = now;

    if (stats_.cpu_name.empty()) {
        stats_.cpu_name = fallback_cpu_label();
    }
    const std::string gpu_label = renderer_description(renderer);
    if (!gpu_label.empty()) {
        stats_.gpu_name = gpu_label;
    } else if (stats_.gpu_name.empty()) {
        stats_.gpu_name = std::string("Unknown Renderer");
    }

    stats_.fps = 0.0;
    if (assets_context_) {
        const double dt = static_cast<double>(assets_context_->frame_delta_seconds());
        if (dt > 0.0001) {
            stats_.fps = 1.0 / dt;
        }
    }

#ifdef _WIN32
    FILETIME ft_creation{}, ft_exit{}, ft_kernel{}, ft_user{};
    if (GetProcessTimes(GetCurrentProcess(), &ft_creation, &ft_exit, &ft_kernel, &ft_user)) {
        const unsigned long long kernel = filetime_to_ull(ft_kernel);
        const unsigned long long user = filetime_to_ull(ft_user);
        if (last_cpu_sample_ms_ > 0 && now > last_cpu_sample_ms_) {
            const double elapsed_sec = static_cast<double>(now - last_cpu_sample_ms_) / 1000.0;
            if (elapsed_sec > 0.0) {
                const unsigned long long kernel_diff = kernel - last_cpu_kernel_time_;
                const unsigned long long user_diff = user - last_cpu_user_time_;
                SYSTEM_INFO sys_info{};
                GetSystemInfo(&sys_info);
                const double cpu_time = static_cast<double>(kernel_diff + user_diff) / 10000000.0;
                const double cores = std::max<DWORD>(1, sys_info.dwNumberOfProcessors);
                stats_.cpu_usage_percent = std::clamp((cpu_time / (elapsed_sec * cores)) * 100.0, 0.0, 100.0);
            }
        }
        last_cpu_kernel_time_ = kernel;
        last_cpu_user_time_ = user;
        last_cpu_sample_ms_ = now;
    }

    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        const std::uint64_t used_bytes = status.ullTotalPhys - status.ullAvailPhys;
        stats_.ram_total_gb = bytes_to_gb(status.ullTotalPhys);
        stats_.ram_used_gb = bytes_to_gb(used_bytes);
    } else {
        stats_.ram_total_gb = 0.0;
        stats_.ram_used_gb = 0.0;
    }
#else
    stats_.cpu_usage_percent = 0.0;
    stats_.ram_total_gb = 0.0;
    stats_.ram_used_gb = 0.0;
#endif

    stats_.lines[0] = std::string("GPU: ") + (stats_.gpu_name.empty() ? std::string("Unknown") : stats_.gpu_name);
    std::ostringstream cpu_line;
    cpu_line << "CPU: " << (stats_.cpu_name.empty() ? std::string("Unknown") : stats_.cpu_name);
    if (stats_.cpu_usage_percent > 0.0) {
        cpu_line << " (" << format_percent(stats_.cpu_usage_percent) << ")";
    }
    stats_.lines[1] = cpu_line.str();
    stats_.lines[2] = format_ram_line(stats_.ram_used_gb, stats_.ram_total_gb);
    stats_.lines[3] = std::string("FPS: ") + format_fps(stats_.fps);
}

void OtherSettingsAndControls::render_stats(SDL_Renderer* renderer) const {
    if (stats_rect_.w <= 0 || stats_rect_.h <= 0) {
        return;
    }
    auto* self = const_cast<OtherSettingsAndControls*>(this);
    self->maybe_refresh_stats(renderer);

    const SDL_Color stats_bg = reduced_alpha_color(DMStyles::PanelBG());
    SDL_SetRenderDrawColor(renderer, stats_bg.r, stats_bg.g, stats_bg.b, stats_bg.a);
    sdl_render::FillRect(renderer, &stats_rect_);

    const int margin_x = DMSpacing::item_gap();
    const int left = stats_rect_.x + margin_x;
    const int right = stats_rect_.x + stats_rect_.w - margin_x;
    const SDL_Color border = DMStyles::Border();
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, 160);
    SDL_RenderLine(renderer, left, stats_rect_.y, right, stats_rect_.y);

    const DMLabelStyle& style = DMStyles::Label();
    if (stats_heading_rect_.w > 0 && stats_heading_rect_.h > 0) {
        DrawLabelText(renderer, kStatsTitle, stats_heading_rect_.x, stats_heading_rect_.y, style);
    }
    for (size_t i = 0; i < stats_.lines.size() && i < stats_line_rects_.size(); ++i) {
        if (stats_line_rects_[i].w <= 0 || stats_.lines[i].empty()) {
            continue;
        }
        DrawLabelText(renderer, stats_.lines[i], stats_line_rects_[i].x, stats_line_rects_[i].y, style);
    }
}

bool OtherSettingsAndControls::type_filter_enabled(const std::string& type) const {
    const FilterState& state_ref = state();
    auto it = state_ref.type_filters.find(type);
    if (it == state_ref.type_filters.end()) {
        return true;
    }
    return it->second;
}

bool OtherSettingsAndControls::method_filter_enabled(const std::string& method) const {
    const FilterState& state_ref = state();
    auto it = state_ref.method_filters.find(method);
    if (it == state_ref.method_filters.end()) {
        return true;
    }
    return it->second;
}

bool OtherSettingsAndControls::load_type_filter_value(const std::string& type, bool default_value) const {
    if (!has_saved_state_) {
        return default_value;
    }
    return devmode::ui_settings::load_bool(make_type_setting_key(type), default_value);
}

bool OtherSettingsAndControls::load_method_filter_value(const std::string& method, bool default_value) const {
    if (!has_saved_state_) {
        return default_value;
    }
    return devmode::ui_settings::load_bool(make_method_setting_key(method), default_value);
}

std::string OtherSettingsAndControls::format_type_label(const std::string& type) const {
    if (type.empty()) {
        return std::string{};
    }
    std::string label = type;
    std::transform(label.begin(), label.end(), label.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return label;
}

std::string OtherSettingsAndControls::format_method_label(const std::string& method) const {
    if (method.empty()) {
        return std::string{};
    }
    std::string label;
    label.reserve(method.size() + 4);
    char prev = '\0';
    for (unsigned char ch : method) {
        if (ch == '_' || ch == '-') {
            if (!label.empty() && label.back() != ' ') {
                label.push_back(' ');
            }
            prev = ch;
            continue;
        }
        if (std::isupper(ch) && !label.empty() &&
            (std::islower(static_cast<unsigned char>(prev)) || std::isdigit(static_cast<unsigned char>(prev)))) {
            label.push_back(' ');
        }
        label.push_back(static_cast<char>(ch));
        prev = static_cast<char>(ch);
    }
    bool start = true;
    for (char& ch : label) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isspace(uch)) {
            start = true;
            continue;
        }
        if (start) {
            ch = static_cast<char>(std::toupper(uch));
            start = false;
        } else {
            ch = static_cast<char>(std::tolower(uch));
        }
    }
    return label;
}

std::string OtherSettingsAndControls::canonicalize_method(const std::string& method) {
    return asset_filters::canonicalize_spawn_method(method);
}

void OtherSettingsAndControls::collect_spawn_ids(const nlohmann::json& node, std::unordered_set<std::string>& out) const {
    if (node.is_object()) {
        auto sg = node.find("spawn_groups");
        if (sg != node.end() && sg->is_array()) {
            for (const auto& entry : *sg) {
                if (!entry.is_object()) {
                    continue;
                }
                auto id_it = entry.find("spawn_id");
                if (id_it != entry.end() && id_it->is_string()) {
                    out.insert(id_it->get<std::string>());
                }
            }
        }
        for (const auto& item : node.items()) {
            if (item.key() == "spawn_groups") {
                continue;
            }
            collect_spawn_ids(item.value(), out);
        }
    } else if (node.is_array()) {
        for (const auto& element : node) {
            collect_spawn_ids(element, out);
        }
    }
}

void OtherSettingsAndControls::load_persisted_state() {
    ensure_persistent_state_loaded();
    FilterState& state_ref = mutable_state();
    state_ref.type_filters.clear();
    state_ref.method_filters.clear();
    has_saved_state_ = persistent_state_initialized_flag();
    if (!has_saved_state_) {
        state_ref.map_assets = true;
        state_ref.current_room = true;
        state_ref.fog = true;
        filters_expanded_ = false;
        return;
    }
    state_ref.map_assets = devmode::ui_settings::load_bool(kSettingsMapAssetsKey, true);
    state_ref.current_room = devmode::ui_settings::load_bool(kSettingsCurrentRoomKey, true);
    state_ref.fog = devmode::ui_settings::load_bool(kSettingsFogKey, true);
    filters_expanded_ = persistent_filters_expanded_flag();
}

void OtherSettingsAndControls::persist_state() {
    FilterState& state_ref = mutable_state();
    devmode::ui_settings::save_bool(kSettingsInitializedKey, true);
    devmode::ui_settings::save_bool(kSettingsMapAssetsKey, state_ref.map_assets);
    devmode::ui_settings::save_bool(kSettingsCurrentRoomKey, state_ref.current_room);
    devmode::ui_settings::save_bool(kSettingsFogKey, state_ref.fog);
    for (const auto& kv : state_ref.type_filters) {
        devmode::ui_settings::save_bool(make_type_setting_key(kv.first), kv.second);
    }
    for (const auto& kv : state_ref.method_filters) {
        devmode::ui_settings::save_bool(make_method_setting_key(kv.first), kv.second);
    }
    has_saved_state_ = true;
    persistent_state_initialized_flag() = true;
}

void OtherSettingsAndControls::persist_filters_expanded() const {
    persistent_filters_expanded_flag() = filters_expanded_;
    devmode::ui_settings::save_bool(kSettingsFiltersExpandedKey, filters_expanded_);
}

void OtherSettingsAndControls::ensure_dev_settings_controls() {
    if (!overlay_grid_checkbox_) {
        overlay_grid_checkbox_ = std::make_unique<DMCheckbox>("Show Grid", dev_mode_settings_.show_grid);
    } else {
        overlay_grid_checkbox_->set_value(dev_mode_settings_.show_grid);
    }

    const int overlay_min = grid_resolution_min_;
    const int overlay_max = grid_resolution_max_ > 0 ? grid_resolution_max_ : std::max(grid_resolution_min_, 10);
    const int clamped_overlay = std::clamp(dev_mode_settings_.overlay_resolution, overlay_min, overlay_max);
    if (!overlay_grid_stepper_) {
        overlay_grid_stepper_ = std::make_unique<DMNumericStepper>("Grid Overlay (r)", overlay_min, overlay_max, clamped_overlay);
    } else {
        overlay_grid_stepper_->set_range(overlay_min, overlay_max);
        overlay_grid_stepper_->set_value(clamped_overlay);
    }

    if (!snap_to_grid_checkbox_) {
        snap_to_grid_checkbox_ = std::make_unique<DMCheckbox>("Grid Snapping", dev_mode_settings_.snap_to_grid);
    } else {
        snap_to_grid_checkbox_->set_value(dev_mode_settings_.snap_to_grid);
    }

    if (!movement_debug_checkbox_) {
        movement_debug_checkbox_ = std::make_unique<DMCheckbox>("Debug Movement", dev_mode_settings_.movement_debug);
    } else {
        movement_debug_checkbox_->set_value(dev_mode_settings_.movement_debug);
    }

    if (!anchor_point_debug_checkbox_) {
        anchor_point_debug_checkbox_ = std::make_unique<DMCheckbox>("Debug Anchor Points", dev_mode_settings_.anchor_point_debug);
    } else {
        anchor_point_debug_checkbox_->set_value(dev_mode_settings_.anchor_point_debug);
    }

    if (!depth_effects_checkbox_) {
        depth_effects_checkbox_ = std::make_unique<DMCheckbox>("Depth Effects", dev_mode_settings_.depth_effects);
    } else {
        depth_effects_checkbox_->set_value(dev_mode_settings_.depth_effects);
    }
}


