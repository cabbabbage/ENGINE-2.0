#pragma once

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace depth_cue {

inline constexpr const char* kMapEntryKey = "depth_cue_settings";
inline constexpr const char* kCenterDepthOffsetKey = "center_depth_offset";
inline constexpr const char* kForegroundMaxDepthOffsetKey = "foreground_max_depth_offset";
inline constexpr const char* kBackgroundMaxDepthOffsetKey = "background_max_depth_offset";

inline constexpr float kMinDepthOffset = -50000.0f;
inline constexpr float kMaxDepthOffset = 50000.0f;
inline constexpr float kMinSeparation = 5.0f;

struct DepthCueSettings {
    float center_depth_offset = 0.0f;
    float foreground_max_depth_offset = -600.0f;
    float background_max_depth_offset = 600.0f;
};

inline bool nearly_equal(const DepthCueSettings& a,
                         const DepthCueSettings& b,
                         float epsilon = 1.0e-4f) {
    return std::fabs(a.center_depth_offset - b.center_depth_offset) <= epsilon &&
           std::fabs(a.foreground_max_depth_offset - b.foreground_max_depth_offset) <= epsilon &&
           std::fabs(a.background_max_depth_offset - b.background_max_depth_offset) <= epsilon;
}

inline void clamp(DepthCueSettings& settings) {
    if (!std::isfinite(settings.center_depth_offset)) {
        settings.center_depth_offset = 0.0f;
    }
    if (!std::isfinite(settings.foreground_max_depth_offset)) {
        settings.foreground_max_depth_offset = -600.0f;
    }
    if (!std::isfinite(settings.background_max_depth_offset)) {
        settings.background_max_depth_offset = 600.0f;
    }

    const float min_center = kMinDepthOffset + kMinSeparation;
    const float max_center = kMaxDepthOffset - kMinSeparation;
    settings.center_depth_offset = std::clamp(settings.center_depth_offset, min_center, max_center);

    settings.foreground_max_depth_offset = std::clamp(settings.foreground_max_depth_offset,
                                                      kMinDepthOffset,
                                                      settings.center_depth_offset - kMinSeparation);
    settings.background_max_depth_offset = std::clamp(settings.background_max_depth_offset,
                                                      settings.center_depth_offset + kMinSeparation,
                                                      kMaxDepthOffset);
}

inline DepthCueSettings from_json_section(const nlohmann::json* section) {
    DepthCueSettings settings{};
    if (!section || !section->is_object()) {
        clamp(settings);
        return settings;
    }

    settings.center_depth_offset = section->value(kCenterDepthOffsetKey, settings.center_depth_offset);
    settings.foreground_max_depth_offset =
        section->value(kForegroundMaxDepthOffsetKey, settings.foreground_max_depth_offset);
    settings.background_max_depth_offset =
        section->value(kBackgroundMaxDepthOffsetKey, settings.background_max_depth_offset);
    clamp(settings);
    return settings;
}

inline DepthCueSettings from_map_entry(const nlohmann::json& map_entry) {
    if (!map_entry.is_object()) {
        DepthCueSettings settings{};
        clamp(settings);
        return settings;
    }

    auto it = map_entry.find(kMapEntryKey);
    return from_json_section(it == map_entry.end() ? nullptr : &(*it));
}

inline nlohmann::json to_json(const DepthCueSettings& input) {
    DepthCueSettings settings = input;
    clamp(settings);

    nlohmann::json out = nlohmann::json::object();
    out[kCenterDepthOffsetKey] = settings.center_depth_offset;
    out[kForegroundMaxDepthOffsetKey] = settings.foreground_max_depth_offset;
    out[kBackgroundMaxDepthOffsetKey] = settings.background_max_depth_offset;
    return out;
}

inline void write_to_map_entry(nlohmann::json& map_entry, const DepthCueSettings& settings) {
    if (!map_entry.is_object()) {
        map_entry = nlohmann::json::object();
    }
    map_entry[kMapEntryKey] = to_json(settings);
}

} // namespace depth_cue

