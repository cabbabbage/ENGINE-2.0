#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

#include "dev_mode/dev_ui_settings.hpp"

namespace devmode::camera_prefs {

inline constexpr std::string_view kDepthCueEnabledSettingKey = "dev_ui.camera.depthcue_enabled";
inline constexpr std::string_view kForegroundTextureOpacitySettingKey = "dev_ui.camera.foreground_texture_max_opacity";
inline constexpr std::string_view kBackgroundTextureOpacitySettingKey = "dev_ui.camera.background_texture_max_opacity";
inline constexpr std::string_view kMinVisibleScreenRatioSettingKey = "dev_ui.camera.min_visible_screen_ratio";
inline constexpr std::string_view kCullMarginSettingKey = "dev_ui.camera.extra_cull_margin";
inline constexpr std::string_view kMetersPer100WorldSettingKey = "dev_ui.camera.meters_per_100_world_px";
inline constexpr std::string_view kRenderQualityPercentSettingKey = "dev_ui.camera.render_quality_percent";
inline constexpr std::string_view kTextureWarpPercentSettingKey = "dev_ui.camera.texture_warp_percent";
inline constexpr std::string_view kTextureWarpYOffsetSettingKey = "dev_ui.camera.texture_warp_y_offset_px";
inline constexpr std::string_view kNearCameraScaleStartRatioSettingKey = "dev_ui.camera.near_camera_scale_start_ratio";
inline constexpr std::string_view kNearCameraScaleEndRatioSettingKey = "dev_ui.camera.near_camera_scale_end_ratio";
inline constexpr std::string_view kNearCameraMaxPerspectiveScaleSettingKey = "dev_ui.camera.near_camera_max_perspective_scale";
inline constexpr std::string_view kNearCameraFadeStartRatioSettingKey = "dev_ui.camera.near_camera_fade_start_ratio";
inline constexpr std::string_view kNearCameraFadeEndRatioSettingKey = "dev_ui.camera.near_camera_fade_end_ratio";

inline bool load_depthcue_enabled() {
    return devmode::ui_settings::load_bool(kDepthCueEnabledSettingKey, false);
}

inline void save_depthcue_enabled(bool enabled) {
    devmode::ui_settings::save_bool(kDepthCueEnabledSettingKey, enabled);
}

inline int load_foreground_texture_max_opacity() {
    const double stored = devmode::ui_settings::load_number(kForegroundTextureOpacitySettingKey, 0.0);
    const double clamped = std::clamp(stored, 0.0, 255.0);
    return static_cast<int>(std::round(clamped));
}

inline void save_foreground_texture_max_opacity(int value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 255.0);
    devmode::ui_settings::save_number(kForegroundTextureOpacitySettingKey, clamped);
}

inline int load_background_texture_max_opacity() {
    const double stored = devmode::ui_settings::load_number(kBackgroundTextureOpacitySettingKey, 0.0);
    const double clamped = std::clamp(stored, 0.0, 255.0);
    return static_cast<int>(std::round(clamped));
}

inline void save_background_texture_max_opacity(int value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 255.0);
    devmode::ui_settings::save_number(kBackgroundTextureOpacitySettingKey, clamped);
}

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

inline float load_texture_warp_percent(float default_value) {
    return static_cast<float>(devmode::ui_settings::load_number(kTextureWarpPercentSettingKey, default_value));
}

inline void save_texture_warp_percent(float value) {
    devmode::ui_settings::save_number(kTextureWarpPercentSettingKey, value);
}

inline float load_texture_warp_y_offset_px(float default_value) {
    return static_cast<float>(devmode::ui_settings::load_number(kTextureWarpYOffsetSettingKey, default_value));
}

inline void save_texture_warp_y_offset_px(float value) {
    devmode::ui_settings::save_number(kTextureWarpYOffsetSettingKey, value);
}

inline float load_near_camera_scale_start_ratio(float default_value) {
    const double stored = devmode::ui_settings::load_number(kNearCameraScaleStartRatioSettingKey, default_value);
    return static_cast<float>(std::clamp(stored, 0.0, 2.0));
}

inline void save_near_camera_scale_start_ratio(float value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 2.0);
    devmode::ui_settings::save_number(kNearCameraScaleStartRatioSettingKey, clamped);
}

inline float load_near_camera_scale_end_ratio(float default_value) {
    const double stored = devmode::ui_settings::load_number(kNearCameraScaleEndRatioSettingKey, default_value);
    return static_cast<float>(std::clamp(stored, 0.0, 2.0));
}

inline void save_near_camera_scale_end_ratio(float value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 2.0);
    devmode::ui_settings::save_number(kNearCameraScaleEndRatioSettingKey, clamped);
}

inline float load_near_camera_max_perspective_scale(float default_value) {
    const double stored = devmode::ui_settings::load_number(kNearCameraMaxPerspectiveScaleSettingKey, default_value);
    return static_cast<float>(std::clamp(stored, 0.0, 100.0));
}

inline void save_near_camera_max_perspective_scale(float value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 100.0);
    devmode::ui_settings::save_number(kNearCameraMaxPerspectiveScaleSettingKey, clamped);
}

inline float load_near_camera_fade_start_ratio(float default_value) {
    const double stored = devmode::ui_settings::load_number(kNearCameraFadeStartRatioSettingKey, default_value);
    return static_cast<float>(std::clamp(stored, 0.0, 2.0));
}

inline void save_near_camera_fade_start_ratio(float value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 2.0);
    devmode::ui_settings::save_number(kNearCameraFadeStartRatioSettingKey, clamped);
}

inline float load_near_camera_fade_end_ratio(float default_value) {
    const double stored = devmode::ui_settings::load_number(kNearCameraFadeEndRatioSettingKey, default_value);
    return static_cast<float>(std::clamp(stored, 0.0, 2.0));
}

inline void save_near_camera_fade_end_ratio(float value) {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 2.0);
    devmode::ui_settings::save_number(kNearCameraFadeEndRatioSettingKey, clamped);
}

}
