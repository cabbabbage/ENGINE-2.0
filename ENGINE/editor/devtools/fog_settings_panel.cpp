#include "fog_settings_panel.hpp"
#include "devtools/dev_ui_settings.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <nlohmann/json.hpp>

namespace {
constexpr float kMinGridMultiplier = 0.25f;
constexpr float kMaxGridMultiplier = 8.0f;
constexpr float kGridStep = 0.25f;

constexpr float kMinBaseScale = 0.25f;
constexpr float kMaxBaseScale = 12.0f;
constexpr float kBaseScaleStep = 0.25f;

constexpr float kMinVerticalOffset = -300.0f;
constexpr float kMaxVerticalOffset = 900.0f;
constexpr float kVerticalOffsetStep = 1.0f;
constexpr float kMinRandomJitter = 0.0f;
constexpr float kMaxRandomJitter = 500.0f;
constexpr float kRandomJitterStep = 1.0f;

constexpr std::string_view kGridSpacingSettingKey = "dev_ui.fog.grid_spacing_multiplier";
constexpr std::string_view kBaseSizeScaleSettingKey = "dev_ui.fog.base_size_scale";
constexpr std::string_view kVerticalOffsetSettingKey = "dev_ui.fog.vertical_offset";
constexpr std::string_view kRandomJitterSettingKey = "dev_ui.fog.max_random_jitter";

float load_saved_setting(std::string_view key, float default_value, float min_value, float max_value) {
    const double stored = devmode::ui_settings::load_number(key, default_value);
    const float as_float = std::isfinite(stored) ? static_cast<float>(stored) : default_value;
    return std::clamp(as_float, min_value, max_value);
}

float clamp_and_save(std::string_view key, float value, float min_value, float max_value) {
    const float clamped = std::clamp(value, min_value, max_value);
    devmode::ui_settings::save_number(key, clamped);
    return clamped;
}

float read_manifest_jitter(nlohmann::json* map_info) {
    if (!map_info || !map_info->is_object()) return 0.0f;
    try {
        auto fog_it = map_info->find("fog_settings");
        if (fog_it == map_info->end() || !fog_it->is_object()) return 0.0f;
        auto jitter_it = fog_it->find("max_random_jitter");
        if (jitter_it == fog_it->end()) return 0.0f;
        if (jitter_it->is_number_float()) return static_cast<float>(jitter_it->get<double>());
        if (jitter_it->is_number_integer()) return static_cast<float>(jitter_it->get<int>());
    } catch (...) {
    }
    return 0.0f;
}
}

FogSettingsPanel::FogSettingsPanel()
    : DockableCollapsible("Fog Settings", /*floatable=*/true, 420, 96) {
    set_close_button_enabled(true);
    set_scroll_enabled(false);
    set_visible_height(180);
}

void FogSettingsPanel::build() {
    const float saved_grid_multiplier = load_saved_setting(
        kGridSpacingSettingKey,
        DynamicFogSystem::kDefaultGridSpacingMultiplier,
        kMinGridMultiplier,
        kMaxGridMultiplier);
    const float saved_base_scale = load_saved_setting(
        kBaseSizeScaleSettingKey,
        DynamicFogSystem::kDefaultBaseSizeScale,
        kMinBaseScale,
        kMaxBaseScale);
    const float saved_vertical_offset = load_saved_setting(
        kVerticalOffsetSettingKey,
        DynamicFogSystem::kDefaultVerticalOffset,
        kMinVerticalOffset,
        kMaxVerticalOffset);
    float saved_random_jitter = 0.0f;
    if (map_info_) {
        saved_random_jitter = std::clamp(read_manifest_jitter(map_info_), kMinRandomJitter, kMaxRandomJitter);
    } else {
        saved_random_jitter = load_saved_setting(
            kRandomJitterSettingKey,
            0.0f,
            kMinRandomJitter,
            kMaxRandomJitter);
    }

    DynamicFogSystem::set_grid_spacing_multiplier(saved_grid_multiplier);
    DynamicFogSystem::set_base_size_scale(saved_base_scale);
    DynamicFogSystem::set_vertical_offset(saved_vertical_offset);
    DynamicFogSystem::set_max_random_jitter(saved_random_jitter);

    grid_spacing_slider_ = std::make_unique<FloatSliderWidget>(
        "Grid Spacing Multiplier",
        kMinGridMultiplier,
        kMaxGridMultiplier,
        kGridStep,
        saved_grid_multiplier,
        2);

    base_scale_slider_ = std::make_unique<FloatSliderWidget>(
        "Base Size Scale",
        kMinBaseScale,
        kMaxBaseScale,
        kBaseScaleStep,
        saved_base_scale,
        2);

    vertical_offset_slider_ = std::make_unique<FloatSliderWidget>(
        "Vertical Offset",
        kMinVerticalOffset,
        kMaxVerticalOffset,
        kVerticalOffsetStep,
        saved_vertical_offset,
        0);

    max_random_jitter_slider_ = std::make_unique<FloatSliderWidget>(
        "Max Random Fog Jitter",
        kMinRandomJitter,
        kMaxRandomJitter,
        kRandomJitterStep,
        saved_random_jitter,
        0);

    grid_spacing_slider_->set_on_value_changed([](float v) {
        const float clamped = clamp_and_save(kGridSpacingSettingKey, v, kMinGridMultiplier, kMaxGridMultiplier);
        DynamicFogSystem::set_grid_spacing_multiplier(clamped);
    });
    base_scale_slider_->set_on_value_changed([](float v) {
        const float clamped = clamp_and_save(kBaseSizeScaleSettingKey, v, kMinBaseScale, kMaxBaseScale);
        DynamicFogSystem::set_base_size_scale(clamped);
    });
    vertical_offset_slider_->set_on_value_changed([](float v) {
        const float clamped = clamp_and_save(kVerticalOffsetSettingKey, v, kMinVerticalOffset, kMaxVerticalOffset);
        DynamicFogSystem::set_vertical_offset(clamped);
    });
    max_random_jitter_slider_->set_on_value_changed([this](float v) {
        const float clamped = std::clamp(v, kMinRandomJitter, kMaxRandomJitter);
        this->set_max_random_jitter_internal(clamped, true);
    });

    Rows rows;
    rows.push_back({grid_spacing_slider_.get()});
    rows.push_back({base_scale_slider_.get()});
    rows.push_back({vertical_offset_slider_.get()});
    rows.push_back({max_random_jitter_slider_.get()});
    set_rows(rows);
}

void FogSettingsPanel::set_map_info(nlohmann::json* map_info, std::function<bool()> on_save) {
    map_info_ = map_info;
    on_save_ = std::move(on_save);
    refresh_from_map();
}

void FogSettingsPanel::set_grid_spacing_multiplier(float multiplier) {
    const float clamped = clamp_and_save(kGridSpacingSettingKey, multiplier, kMinGridMultiplier, kMaxGridMultiplier);
    if (grid_spacing_slider_) {
        grid_spacing_slider_->set_value(clamped);
    }
    DynamicFogSystem::set_grid_spacing_multiplier(clamped);
}

void FogSettingsPanel::set_base_size_scale(float scale) {
    const float clamped = clamp_and_save(kBaseSizeScaleSettingKey, scale, kMinBaseScale, kMaxBaseScale);
    if (base_scale_slider_) {
        base_scale_slider_->set_value(clamped);
    }
    DynamicFogSystem::set_base_size_scale(clamped);
}

void FogSettingsPanel::set_vertical_offset(float offset) {
    const float clamped = clamp_and_save(kVerticalOffsetSettingKey, offset, kMinVerticalOffset, kMaxVerticalOffset);
    if (vertical_offset_slider_) {
        vertical_offset_slider_->set_value(clamped);
    }
    DynamicFogSystem::set_vertical_offset(clamped);
}

void FogSettingsPanel::set_max_random_jitter(float jitter) {
    const float clamped = std::clamp(jitter, kMinRandomJitter, kMaxRandomJitter);
    set_max_random_jitter_internal(clamped, false);
}

void FogSettingsPanel::set_max_random_jitter_internal(float jitter, bool persist_dev_setting) {
    const float clamped = std::clamp(jitter, kMinRandomJitter, kMaxRandomJitter);
    if (persist_dev_setting) {
        devmode::ui_settings::save_number(kRandomJitterSettingKey, clamped);
    }
    if (max_random_jitter_slider_) {
        max_random_jitter_slider_->set_value(clamped);
    }
    if (map_info_) {
        nlohmann::json& fog_settings = ensure_fog_settings();
        fog_settings["max_random_jitter"] = clamped;
        if (on_save_) {
            on_save_();
        }
    }
    DynamicFogSystem::set_max_random_jitter(clamped);
}

nlohmann::json& FogSettingsPanel::ensure_fog_settings() {
    if (!map_info_) {
        static nlohmann::json empty = nlohmann::json::object();
        return empty;
    }
    if (!map_info_->is_object()) {
        *map_info_ = nlohmann::json::object();
    }
    auto it = map_info_->find("fog_settings");
    if (it == map_info_->end() || !it->is_object()) {
        (*map_info_)["fog_settings"] = nlohmann::json::object();
    }
    return (*map_info_)["fog_settings"];
}

void FogSettingsPanel::refresh_from_map() {
    float jitter = map_info_ ? std::clamp(read_manifest_jitter(map_info_), kMinRandomJitter, kMaxRandomJitter)
                             : load_saved_setting(kRandomJitterSettingKey, 0.0f, kMinRandomJitter, kMaxRandomJitter);
    if (max_random_jitter_slider_) {
        max_random_jitter_slider_->set_value(jitter);
    }
    DynamicFogSystem::set_max_random_jitter(jitter);
}
