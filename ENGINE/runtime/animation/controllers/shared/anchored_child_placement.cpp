#include "animation/controllers/shared/anchored_child_placement.hpp"

#include <algorithm>
#include <cmath>

#include "rendering/render/projected_sprite_frame.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace anchored_child_placement {
namespace {

constexpr float kEpsilon = 1.0e-6f;
constexpr float kPi = 3.14159265358979323846f;

float sanitize_scale(float value) {
    if (!std::isfinite(value) || std::fabs(value) < kEpsilon) {
        return 1.0f;
    }
    return value;
}

bool is_valid_world_point(const anchor_points::AnchorWorldPoint3& point) {
    return point.valid &&
           std::isfinite(point.x) &&
           std::isfinite(point.y) &&
           std::isfinite(point.z);
}

bool project_world_point(const WarpedScreenGrid* cam,
                         const anchor_points::AnchorWorldPoint3& world,
                         SDL_FPoint& out_screen) {
    if (!cam || !is_valid_world_point(world)) {
        return false;
    }

    render_projection::WorldPoint3 world_point{world.x, world.y, world.z, true};
    if (render_projection::project_world_to_screen(*cam, world_point, out_screen)) {
        return std::isfinite(out_screen.x) && std::isfinite(out_screen.y);
    }

    out_screen = cam->map_to_screen_f(SDL_FPoint{world.x, world.y});
    return std::isfinite(out_screen.x) && std::isfinite(out_screen.y);
}

} // namespace

bool resolve_child_placement(const PlacementInput& input, PlacementOutput& output) {
    output = PlacementOutput{};

    const AnchorPoint& anchor = input.anchor_definition.anchor;
    if (!anchor.is_active() ||
        !std::isfinite(anchor.world_exact_pos_2d.x) ||
        !std::isfinite(anchor.world_exact_pos_2d.y) ||
        !std::isfinite(anchor.world_depth)) {
        return false;
    }

    output.anchor_world = anchor_points::AnchorWorldPoint3{
        anchor.world_exact_pos_2d.x,
        anchor.world_exact_pos_2d.y,
        anchor.world_depth,
        true};
    output.resolution_layer = anchor.resolution_layer >= 0
        ? anchor.resolution_layer
        : input.parent.resolution_layer;

    // Explicit operation order for local child placement:
    // 1) mirror -> 2) scale -> 3) rotate -> 4) translate by anchor world.
    float local_x = input.child_offset.x;
    float local_y = input.child_offset.y;
    float local_z = input.child_offset.z;
    if (input.sprite_transform.mirror_x) {
        local_x = -local_x;
    }
    if (input.sprite_transform.mirror_y) {
        local_y = -local_y;
    }

    local_x *= sanitize_scale(input.sprite_transform.scale_x);
    local_y *= sanitize_scale(input.sprite_transform.scale_y);
    const float rotation_deg = std::isfinite(input.sprite_transform.rotation_degrees)
        ? input.sprite_transform.rotation_degrees
        : 0.0f;
    const float radians = rotation_deg * (kPi / 180.0f);
    const float cos_theta = std::cos(radians);
    const float sin_theta = std::sin(radians);
    const float rotated_x = local_x * cos_theta - local_y * sin_theta;
    const float rotated_y = local_x * sin_theta + local_y * cos_theta;

    output.child_world = anchor_points::AnchorWorldPoint3{
        output.anchor_world.x + rotated_x,
        output.anchor_world.y + rotated_y,
        output.anchor_world.z + local_z,
        true};
    if (!is_valid_world_point(output.child_world)) {
        output.child_world.valid = false;
        return false;
    }

    output.child_world_quantized = SDL_Point{
        static_cast<int>(std::lround(output.child_world.x)),
        static_cast<int>(std::lround(output.child_world.y))};
    output.child_world_quantized_z = static_cast<int>(std::lround(output.child_world.z));

    if (input.camera_state.camera) {
        output.has_anchor_screen_px =
            project_world_point(input.camera_state.camera, output.anchor_world, output.anchor_screen_px);
        output.has_child_screen_px =
            project_world_point(input.camera_state.camera, output.child_world, output.child_screen_px);
    }

    return true;
}

bool project_child_pivot_screen(const WarpedScreenGrid& cam,
                                float world_x,
                                float world_y,
                                float world_z,
                                SDL_FPoint& out_screen) {
    anchor_points::AnchorWorldPoint3 world{world_x, world_y, world_z, true};
    return project_world_point(&cam, world, out_screen);
}

} // namespace anchored_child_placement
