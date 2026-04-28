#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace oval_anchor_math {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInherentAngleOffsetDegrees = 45.0f;

inline float normalize_angle_degrees(float degrees) {
    if (!std::isfinite(degrees)) {
        return 0.0f;
    }
    float normalized = std::fmod(degrees, 360.0f);
    if (normalized < 0.0f) {
        normalized += 360.0f;
    }
    if (normalized >= 360.0f) {
        normalized -= 360.0f;
    }
    return normalized;
}

inline float radians_to_degrees(float radians) {
    if (!std::isfinite(radians)) {
        return 0.0f;
    }
    return normalize_angle_degrees(radians * (180.0f / kPi));
}

inline float angle_degrees_from_xz_vector(float x, float z) {
    if (!std::isfinite(x) || !std::isfinite(z)) {
        return 0.0f;
    }
    const float raw_angle = static_cast<float>(std::atan2(z, x) * (180.0 / kPi));
    return normalize_angle_degrees(raw_angle - kInherentAngleOffsetDegrees);
}

inline int rounded_int(float value) {
    if (!std::isfinite(value)) {
        return 0;
    }
    if (value > static_cast<float>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    if (value < static_cast<float>(std::numeric_limits<int>::min())) {
        return std::numeric_limits<int>::min();
    }
    return static_cast<int>(std::lround(value));
}

inline void compute_xz_offsets_from_angle(float angle_degrees,
                                          float width_radius_x,
                                          float height_radius_z,
                                          int& out_offset_x,
                                          int& out_offset_z) {
    const float clamped_width = (std::isfinite(width_radius_x) && width_radius_x > 0.0f)
        ? width_radius_x
        : 1.0f;
    const float clamped_height = (std::isfinite(height_radius_z) && height_radius_z > 0.0f)
        ? height_radius_z
        : 1.0f;
    const float radians =
        normalize_angle_degrees(angle_degrees + kInherentAngleOffsetDegrees) * (kPi / 180.0f);
    out_offset_x = rounded_int(std::cos(radians) * clamped_width);
    out_offset_z = rounded_int(std::sin(radians) * clamped_height);
}

struct WorldPoint3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool valid = false;
};

inline WorldPoint3 make_flat_xz_point(float center_x,
                                      float center_y,
                                      float center_z,
                                      int offset_x,
                                      int offset_z) {
    WorldPoint3 point{};
    point.x = center_x + static_cast<float>(offset_x);
    point.y = center_y;
    point.z = center_z + static_cast<float>(offset_z);
    point.valid = std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    return point;
}

inline WorldPoint3 apply_vertical_offset(const WorldPoint3& flat_point, float vertical_offset) {
    WorldPoint3 point = flat_point;
    if (!flat_point.valid || !std::isfinite(vertical_offset)) {
        point.valid = false;
        return point;
    }
    point.y = flat_point.y + vertical_offset;
    point.valid = std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    return point;
}

}  // namespace oval_anchor_math

