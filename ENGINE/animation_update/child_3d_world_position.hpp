#pragma once

#include <SDL.h>
#include <cmath>

namespace animation_update::child_3d {

/**
 * Represents a 3D world position where Z-axis is up/down (vertical depth).
 * X/Y coordinates are in screen/world space, Z is depth for layering.
 */
struct WorldPosition3D {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    WorldPosition3D() = default;

    explicit WorldPosition3D(float x_, float y_, float z_ = 0.0f)
        : x(x_), y(y_), z(z_) {}

    explicit WorldPosition3D(const SDL_FPoint& pos, float z_ = 0.0f)
        : x(pos.x), y(pos.y), z(z_) {}

    [[nodiscard]] SDL_FPoint to_2d() const {
        return SDL_FPoint{x, y};
    }

    [[nodiscard]] bool is_valid() const {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
};

/**
 * Parent state information needed to calculate child 3D world positions.
 * Encapsulates all necessary parent data for consistent transformations.
 */
struct Parent3DState {
    // Parent's anchor point (typically bottom-middle) in world space
    SDL_FPoint anchor_world{0.0f, 0.0f};

    // Parent's Z-depth for layering
    float z_depth = 0.0f;

    // Parent's scale factor that applies to child displacements
    float scale = 1.0f;

    // Whether parent is flipped horizontally
    bool flipped = false;

    Parent3DState() = default;

    Parent3DState(const SDL_FPoint& anchor, float z, float s, bool flip)
        : anchor_world(anchor), z_depth(z), scale(s), flipped(flip) {}

    [[nodiscard]] bool is_valid() const {
        return std::isfinite(anchor_world.x) && std::isfinite(anchor_world.y) &&
               std::isfinite(z_depth) && std::isfinite(scale) && scale > 0.0f;
    }
};

/**
 * Child 3D displacement data (local to parent).
 * Represents how far the child is positioned relative to parent's anchor.
 */
struct Child3DDisplacement {
    float dx = 0.0f;  // X displacement from parent anchor (screen-space units)
    float dy = 0.0f;  // Y displacement from parent anchor (screen-space units)
    float dz = 0.0f;  // Z displacement from parent Z (depth)

    Child3DDisplacement() = default;

    explicit Child3DDisplacement(float dx_, float dy_, float dz_ = 0.0f)
        : dx(dx_), dy(dy_), dz(dz_) {}

    [[nodiscard]] bool is_valid() const {
        return std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz);
    }
};

/**
 * Calculates the 3D world position of a child asset.
 *
 * The calculation works as follows:
 * 1. Scale child displacement by parent scale
 * 2. Mirror X displacement if parent is flipped
 * 3. Add scaled/mirrored displacement to parent anchor
 * 4. Add scaled Z displacement to parent Z depth
 *
 * @param parent_state  Parent's 3D state (anchor, Z, scale, flip)
 * @param displacement  Child's local 3D displacement from parent
 * @return              Absolute 3D world position of the child
 */
[[nodiscard]] inline WorldPosition3D calculate_child_world_position(
    const Parent3DState& parent_state,
    const Child3DDisplacement& displacement) {

    if (!parent_state.is_valid() || !displacement.is_valid()) {
        return WorldPosition3D{};
    }

    // Scale displacement by parent scale
    const float scaled_dx = displacement.dx * parent_state.scale;
    const float scaled_dy = displacement.dy * parent_state.scale;
    const float scaled_dz = displacement.dz * parent_state.scale;

    // Mirror X displacement if parent is flipped
    const float final_dx = parent_state.flipped ? -scaled_dx : scaled_dx;

    // Calculate world position: parent anchor + scaled displacement
    const float world_x = parent_state.anchor_world.x + final_dx;
    const float world_y = parent_state.anchor_world.y + scaled_dy;
    const float world_z = parent_state.z_depth + scaled_dz;

    return WorldPosition3D{world_x, world_y, world_z};
}

/**
 * Alternative: Calculates child 3D position from parent state components directly.
 * Useful when you have individual values rather than structs.
 */
[[nodiscard]] inline WorldPosition3D calculate_child_world_position(
    const SDL_FPoint& parent_anchor,
    float parent_z,
    float parent_scale,
    bool parent_flipped,
    float child_dx,
    float child_dy,
    float child_dz) {

    return calculate_child_world_position(
        Parent3DState{parent_anchor, parent_z, parent_scale, parent_flipped},
        Child3DDisplacement{child_dx, child_dy, child_dz}
    );
}

/**
 * Converts a child's local 3D displacement to world position incrementally.
 * Useful for editor UI where you need to understand the transformation steps.
 *
 * @param parent_state  Parent's 3D state
 * @param displacement  Child's local displacement
 * @param out_scaled    [output] Displacement after scaling (but before parent anchor)
 * @return              Absolute 3D world position
 */
[[nodiscard]] inline WorldPosition3D calculate_child_world_position_with_steps(
    const Parent3DState& parent_state,
    const Child3DDisplacement& displacement,
    Child3DDisplacement& out_scaled) {

    if (!parent_state.is_valid() || !displacement.is_valid()) {
        return WorldPosition3D{};
    }

    // Step 1: Scale
    out_scaled.dx = displacement.dx * parent_state.scale;
    out_scaled.dy = displacement.dy * parent_state.scale;
    out_scaled.dz = displacement.dz * parent_state.scale;

    // Step 2: Mirror if flipped
    const float final_dx = parent_state.flipped ? -out_scaled.dx : out_scaled.dx;

    // Step 3: Apply parent anchor
    return WorldPosition3D{
        parent_state.anchor_world.x + final_dx,
        parent_state.anchor_world.y + out_scaled.dy,
        parent_state.z_depth + out_scaled.dz
    };
}

}  // namespace animation_update::child_3d
