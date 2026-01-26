#include "fog_settings_panel.hpp"

#include <algorithm>

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
}

FogSettingsPanel::FogSettingsPanel()
    : DockableCollapsible("Fog Settings", /*floatable=*/true, 420, 96) {
    set_close_button_enabled(true);
    set_scroll_enabled(false);
    set_visible_height(180);
}

void FogSettingsPanel::build() {
    grid_spacing_slider_ = std::make_unique<FloatSliderWidget>(
        "Grid Spacing Multiplier",
        kMinGridMultiplier,
        kMaxGridMultiplier,
        kGridStep,
        DynamicFogSystem::grid_spacing_multiplier(),
        2);

    base_scale_slider_ = std::make_unique<FloatSliderWidget>(
        "Base Size Scale",
        kMinBaseScale,
        kMaxBaseScale,
        kBaseScaleStep,
        DynamicFogSystem::base_size_scale(),
        2);

    vertical_offset_slider_ = std::make_unique<FloatSliderWidget>(
        "Vertical Offset",
        kMinVerticalOffset,
        kMaxVerticalOffset,
        kVerticalOffsetStep,
        DynamicFogSystem::vertical_offset(),
        0);

    grid_spacing_slider_->set_on_value_changed([](float v) {
        DynamicFogSystem::set_grid_spacing_multiplier(v);
    });
    base_scale_slider_->set_on_value_changed([](float v) {
        DynamicFogSystem::set_base_size_scale(v);
    });
    vertical_offset_slider_->set_on_value_changed([](float v) {
        DynamicFogSystem::set_vertical_offset(v);
    });

    Rows rows;
    rows.push_back({grid_spacing_slider_.get()});
    rows.push_back({base_scale_slider_.get()});
    rows.push_back({vertical_offset_slider_.get()});
    set_rows(rows);
}

void FogSettingsPanel::set_grid_spacing_multiplier(float multiplier) {
    const float clamped = std::clamp(multiplier, kMinGridMultiplier, kMaxGridMultiplier);
    if (grid_spacing_slider_) {
        grid_spacing_slider_->set_value(clamped);
    }
    DynamicFogSystem::set_grid_spacing_multiplier(clamped);
}

void FogSettingsPanel::set_base_size_scale(float scale) {
    const float clamped = std::clamp(scale, kMinBaseScale, kMaxBaseScale);
    if (base_scale_slider_) {
        base_scale_slider_->set_value(clamped);
    }
    DynamicFogSystem::set_base_size_scale(clamped);
}

void FogSettingsPanel::set_vertical_offset(float offset) {
    const float clamped = std::clamp(offset, kMinVerticalOffset, kMaxVerticalOffset);
    if (vertical_offset_slider_) {
        vertical_offset_slider_->set_value(clamped);
    }
    DynamicFogSystem::set_vertical_offset(clamped);
}
