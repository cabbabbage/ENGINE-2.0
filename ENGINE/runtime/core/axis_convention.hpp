#pragma once

// Central axis convention for the engine runtime and dev tools.
// Right-handed system:
//   X: right
//   Y: up (height)
//   Z: forward/depth
//
// All new code must follow this convention. Old Y-forward / Z-up usages are
// considered legacy and should be removed during the refactor.

#include <cstdint>

namespace axis {

// Simple tagged axis identifiers to reduce accidental misuse.
enum class Axis : std::uint8_t { X = 0, Y = 1, Z = 2 };

inline constexpr bool is_height(Axis a) { return a == Axis::Y; }
inline constexpr bool is_depth(Axis a) { return a == Axis::Z; }
inline constexpr bool is_right(Axis a) { return a == Axis::X; }

// Canonical world position in pixels/meters depending on context.
struct WorldPos {
    int x = 0; // right
    int y = 0; // up / height
    int z = 0; // forward / depth
};

// Basic orientation container for camera/objects (row basis vectors).
struct Basis3 {
    double right_x = 1.0, right_y = 0.0, right_z = 0.0;
    double up_x    = 0.0, up_y    = 1.0, up_z    = 0.0;
    double fwd_x   = 0.0, fwd_y   = 0.0, fwd_z   = 1.0;
};

// Helper swizzles to migrate legacy coordinates (old: X right, Y forward, Z up).
inline constexpr WorldPos from_legacy_y_forward_z_up(int x, int y_forward, int z_up) {
    return WorldPos{ x, z_up, y_forward };
}

inline constexpr WorldPos to_legacy_y_forward_z_up(const WorldPos& p) {
    return WorldPos{ p.x, p.z, p.y };
}

// Compile-time guard: if you see a build error referencing this constant,
// you are still using the legacy axis ordering somewhere.
inline constexpr bool kUsingLegacyAxisOrdering = false;

} // namespace axis

