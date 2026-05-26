#pragma once

#include "core/axis_convention.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>

// קבועים וכלי עזר למדיניות עומק הרינדור.
//
// המנוע מתייחס למרחב העולם כמערכת ימנית: X נע ימינה, Y הוא גובה והיסט כלפי מעלה,
// ו-Z מייצג קדימה או עומק ביחס למצלמה. World.Z קובע את סדר הציור בעוד world.Y
// עוקב אחר מיקום אנכי והיסטי פרספקטיבה (למשל כמה גבוה אובייקט נמצא מעל הקרקע).
//
// קובץ זה מרכז את המדיניות כדי ש-renderers יקראו את אותן הגדרות צירים וישמרו על חישובי עומק
// עקביים עם כיוון הצירים הקנוני.

namespace render_depth {

enum class DynamicDepthBand : std::uint8_t {
    FullUpdate = 0,
    PausedFogged = 1,
    Culled = 2,
};

static constexpr axis::Axis kVerticalPlacementAxis = axis::Axis::Y;
static constexpr axis::Axis kOrderingAxis = axis::Axis::Z;

inline float normalize_depth_axis_sign(float sign) {
    constexpr float kDepthAxisForwardEpsilon = 1.0e-5f;
    if (!std::isfinite(sign) || std::fabs(sign) < kDepthAxisForwardEpsilon) {
        return 1.0f;
    }
    return sign >= 0.0f ? 1.0f : -1.0f;
}

inline float world_z_from_depth_offset(float depth_offset, float anchor_world_z, float depth_axis_sign) {
    if (!std::isfinite(depth_offset) || !std::isfinite(anchor_world_z)) {
        return anchor_world_z;
    }
    const float sign = normalize_depth_axis_sign(depth_axis_sign);
    return anchor_world_z + depth_offset * sign;
}

inline double depth_from_anchor(double anchor_depth, double object_depth, double bias = 0.0) {
    return anchor_depth - object_depth + bias;
}

inline DynamicDepthBand classify_dynamic_depth_band(double depth_distance,
                                                    double efficiency_depth,
                                                    double max_cull_depth) {
    if (!std::isfinite(depth_distance)) {
        return DynamicDepthBand::Culled;
    }
    const double clamped_efficiency = std::max(0.0, efficiency_depth);
    const double clamped_max_cull = std::max(1.0, max_cull_depth);
    if (depth_distance > clamped_max_cull) {
        return DynamicDepthBand::Culled;
    }
    if (depth_distance < clamped_efficiency) {
        return DynamicDepthBand::FullUpdate;
    }
    return DynamicDepthBand::PausedFogged;
}

inline double bias_for_quantized_depth(double exact_object_depth, double quantized_object_depth) {
    if (!std::isfinite(exact_object_depth) || !std::isfinite(quantized_object_depth)) {
        return 0.0;
    }
    return quantized_object_depth - exact_object_depth;
}

} // namespace render_depth

