#pragma once

#include <SDL3/SDL.h>

#include <cmath>

#include "animation/controllers/shared/oval_anchor_heading.hpp"
#include "assets/asset/animation_frame.hpp"
#include "core/axis_convention.hpp"

namespace animation_update::movement_rotation {

inline SDL_Point frame_floor_delta_absolute_yaw(const AnimationFrame& frame) {
    float step_x = static_cast<float>(frame.dx);
    float step_z = static_cast<float>(frame.dz);
    const float yaw_degrees = std::isfinite(frame.rotation_degrees) ? frame.rotation_degrees : 0.0f;
    if (std::abs(yaw_degrees) > 1e-5f) {
        const float yaw_radians =
            yaw_degrees * static_cast<float>(3.14159265358979323846 / 180.0);
        oval_anchor_heading::rotate_xz_about_world_y(yaw_radians, step_x, step_z);
    }
    return SDL_Point{
        static_cast<int>(std::lround(step_x)),
        static_cast<int>(std::lround(step_z))
    };
}

inline axis::WorldPos frame_world_delta_absolute_yaw(const AnimationFrame& frame) {
    const SDL_Point rotated_floor = frame_floor_delta_absolute_yaw(frame);
    return axis::WorldPos{rotated_floor.x, frame.dy, rotated_floor.y};
}

}  // namespace animation_update::movement_rotation

