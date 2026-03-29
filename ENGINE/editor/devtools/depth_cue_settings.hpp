#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

#include "devtools/dev_ui_settings.hpp"

namespace devmode::camera_prefs {

inline constexpr std::string_view kMinVisibleScreenRatioSettingKey = "dev_ui.camera.min_visible_screen_ratio";
inline constexpr std::string_view kCullMarginSettingKey = "dev_ui.camera.extra_cull_margin";
inline constexpr std::string_view kBoundaryMinVisibleScreenRatioSettingKey = "dev_ui.camera.boundary_min_visible_screen_ratio";



inline float load_min_visible_screen_ratio(float default_value) {
    return static_cast<float>(devmode::ui_settings::load_number(kMinVisibleScreenRatioSettingKey, default_value));
}

inline void save_min_visible_screen_ratio(float value) {
    devmode::ui_settings::save_number(kMinVisibleScreenRatioSettingKey, value);
}

inline float load_boundary_min_visible_screen_ratio(float default_value) {
    return static_cast<float>(
        devmode::ui_settings::load_number(kBoundaryMinVisibleScreenRatioSettingKey, default_value));
}

inline void save_boundary_min_visible_screen_ratio(float value) {
    devmode::ui_settings::save_number(kBoundaryMinVisibleScreenRatioSettingKey, value);
}

inline float load_extra_cull_margin(float default_value) {
    return static_cast<float>(devmode::ui_settings::load_number(kCullMarginSettingKey, default_value));
}

inline void save_extra_cull_margin(float value) {
    devmode::ui_settings::save_number(kCullMarginSettingKey, value);
}

}
