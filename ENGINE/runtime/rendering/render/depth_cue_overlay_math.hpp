#pragma once

#include <algorithm>
#include <cmath>

#include "core/manifest/depth_cue_settings.hpp"

namespace depth_cue {

enum class OverlayLayer {
    None,
    Foreground,
    Background
};

struct OverlayOpacityDecision {
    OverlayLayer layer = OverlayLayer::None;
    float opacity = 0.0f;
};

inline OverlayOpacityDecision evaluate_overlay_opacity(float signed_depth,
                                                       const DepthCueSettings& input_settings) {
    if (!std::isfinite(signed_depth)) {
        return {};
    }

    DepthCueSettings settings = input_settings;
    clamp(settings);

    const float fg_max = settings.foreground_max_depth_offset;
    const float center = settings.center_depth_offset;
    const float bg_max = settings.background_max_depth_offset;

    if (signed_depth <= fg_max) {
        return {OverlayLayer::Foreground, 1.0f};
    }
    if (signed_depth < center) {
        const float denom = std::max(1.0e-6f, center - fg_max);
        const float t = std::clamp((signed_depth - fg_max) / denom, 0.0f, 1.0f);
        return {OverlayLayer::Foreground, std::clamp(1.0f - t, 0.0f, 1.0f)};
    }
    if (signed_depth > center && signed_depth < bg_max) {
        const float denom = std::max(1.0e-6f, bg_max - center);
        const float t = std::clamp((signed_depth - center) / denom, 0.0f, 1.0f);
        return {OverlayLayer::Background, t};
    }
    if (signed_depth >= bg_max) {
        return {OverlayLayer::Background, 1.0f};
    }

    return {};
}

} // namespace depth_cue

