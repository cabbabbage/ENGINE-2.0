#include "fog_settings_panel.hpp"
#include "dev_mode/dev_ui_settings.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace {
constexpr float kMinGridMultiplier = 0.25f;
constexpr float kMaxGridMultiplier = 8.0f;
constexpr float kGridStep = 0.25f;

constexpr float kMinBaseScale = 0.25f;
constexpr float kMaxBaseScale = 12.0f;
constexpr float kBaseScaleStep = 0.25f;

constexpr float kMinVerticalOffset = -300.0f;
constexpr float kMaxVerticalOffset = 300.0f;
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
    const float saved_random_jitter = load_saved_setting(
        kRandomJitterSettingKey,
        0.0f,
        kMinRandomJitter,
        kMaxRandomJitter);

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
    max_random_jitter_slider_->set_on_value_changed([](float v) {
        const float clamped = clamp_and_save(kRandomJitterSettingKey, v, kMinRandomJitter, kMaxRandomJitter);
        DynamicFogSystem::set_max_random_jitter(clamped);
    });

    Rows rows;
    rows.push_back({grid_spacing_slider_.get()});
    rows.push_back({base_scale_slider_.get()});
    rows.push_back({vertical_offset_slider_.get()});
    rows.push_back({max_random_jitter_slider_.get()});
    set_rows(rows);
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
    const float clamped = clamp_and_save(kRandomJitterSettingKey, jitter, kMinRandomJitter, kMaxRandomJitter);
    if (max_random_jitter_slider_) {
        max_random_jitter_slider_->set_value(clamped);
    }
    DynamicFogSystem::set_max_random_jitter(clamped);
}
