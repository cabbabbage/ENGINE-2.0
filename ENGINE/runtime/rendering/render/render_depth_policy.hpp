#pragma once

#include "core/axis_convention.hpp"

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

static constexpr axis::Axis kVerticalPlacementAxis = axis::Axis::Y;
static constexpr axis::Axis kOrderingAxis = axis::Axis::Z;

inline double depth_from_anchor(double anchor_depth, double object_depth, double bias = 0.0) {
    return anchor_depth - object_depth + bias;
}

inline double bias_for_quantized_depth(double exact_object_depth, double quantized_object_depth) {
    if (!std::isfinite(exact_object_depth) || !std::isfinite(quantized_object_depth)) {
        return 0.0;
    }
    return quantized_object_depth - exact_object_depth;
}

} // namespace render_depth

