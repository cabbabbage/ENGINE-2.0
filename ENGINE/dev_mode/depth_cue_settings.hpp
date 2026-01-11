#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

#include "dev_mode/dev_ui_settings.hpp"

namespace devmode::camera_prefs {

inline constexpr std::string_view kMinVisibleScreenRatioSettingKey = "dev_ui.camera.min_visible_screen_ratio";
inline constexpr std::string_view kCullMarginSettingKey = "dev_ui.camera.extra_cull_margin";
inline constexpr std::string_view kMetersPer100WorldSettingKey = "dev_ui.camera.meters_per_100_world_px";
inline constexpr std::string_view kRenderQualityPercentSettingKey = "dev_ui.camera.render_quality_percent";
inline constexpr std::string_view kNearCameraMaxPerspectiveScaleSettingKey = "dev_ui.camera.near_camera_max_perspective_scale";
inline constexpr std::string_view kOffscreenFadeAmountSettingKey = "dev_ui.camera.offscreen_fade_amount_px";



inline float load_min_visible_screen_ratio(float default_value) {
    return static_cast<float>(devmode::ui_settings::load_number(kMinVisibleScreenRatioSettingKey, default_value));
}

inline void save_min_visible_screen_ratio(float value) {
    devmode::ui_settings::save_number(kMinVisibleScreenRatioSettingKey, value);
}

inline float load_extra_cull_margin(float default_value) {
    return static_cast<float>(devmode::ui_settings::load_number(kCullMarginSettingKey, default_value));
}

inline void save_extra_cull_margin(float value) {
    devmode::ui_settings::save_number(kCullMarginSettingKey, value);
}

inline float load_meters_per_100_world_px(float default_value) {
    return static_cast<float>(devmode::ui_settings::load_number(kMetersPer100WorldSettingKey, default_value));
}

inline void save_meters_per_100_world_px(float value) {
    devmode::ui_settings::save_number(kMetersPer100WorldSettingKey, value);
}

inline int load_render_quality_percent(int default_value) {
    const double stored = devmode::ui_settings::load_number(kRenderQualityPercentSettingKey, default_value);
    return static_cast<int>(std::round(std::clamp(stored, 0.0, 100.0)));
}

inline void save_render_quality_percent(int value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 100.0);
    devmode::ui_settings::save_number(kRenderQualityPercentSettingKey, clamped);
}



inline float load_near_camera_max_perspective_scale(float default_value) {
    const double stored = devmode::ui_settings::load_number(kNearCameraMaxPerspectiveScaleSettingKey, default_value);
    return static_cast<float>(std::clamp(stored, 0.0, 100.0));
}

inline void save_near_camera_max_perspective_scale(float value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 100.0);
    devmode::ui_settings::save_number(kNearCameraMaxPerspectiveScaleSettingKey, clamped);
}

inline float load_offscreen_fade_amount_px(float default_value) {
    const double stored = devmode::ui_settings::load_number(kOffscreenFadeAmountSettingKey, default_value);
    return static_cast<float>(std::clamp(stored, 0.0, 1000.0));
}

inline void save_offscreen_fade_amount_px(float value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 1000.0);
    devmode::ui_settings::save_number(kOffscreenFadeAmountSettingKey, clamped);
}

}
