#pragma once

#include "core/axis_convention.hpp"

// Render depth policy constants and helpers.
//
// The engine treats world space as a right-handed system: X moves right, Y is height and upward displacement,
// and Z represents forward/depth relative to the camera. World.Z drives draw-order sorting while world.Y
// tracks vertical placement/perspective offsets (e.g. how tall an object sits above the terrain).
//
// This file centralizes that policy so renderers can read the same axis definitions and keep depth math
// consistent with the canonical axis orientation.

namespace render_depth {

static constexpr axis::Axis kVerticalPlacementAxis = axis::Axis::Y;
static constexpr axis::Axis kOrderingAxis = axis::Axis::Z;

inline double depth_from_anchor(double anchor_depth, double object_depth, double bias = 0.0) {
    return anchor_depth - object_depth + bias;
}

} // namespace render_depth
