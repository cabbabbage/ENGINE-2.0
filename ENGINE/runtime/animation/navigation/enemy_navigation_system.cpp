#include "animation/navigation/enemy_navigation_system.hpp"

#include <algorithm>
#include <array>

namespace animation_update::navigation {

namespace {

long long horizontal_distance_sq(const axis::WorldPos& a, const axis::WorldPos& b) {
    const long long dx = static_cast<long long>(b.x) - a.x;
    const long long dz = static_cast<long long>(b.z) - a.z;
    return (dx * dx) + (dz * dz);
}

bool crowded(const axis::WorldPos& point, const std::vector<axis::WorldPos>& occupied) {
    for (const axis::WorldPos& other : occupied) {
        if (horizontal_distance_sq(point, other) < 28LL * 28LL) {
            return true;
        }
    }
    return false;
}

} // namespace

EnemyNavigationResult EnemyNavigationSystem::choose_local_waypoint(
    const EnemyNavigationRequest& request) {
    EnemyNavigationResult result{};
    result.local_waypoint = request.goal.target_position;
    if (request.goal.kind == ::animation_update::custom_controllers::internal::MovementGoalKind::None) {
        result.status = ::animation_update::custom_controllers::internal::MovementGoalStatus::None;
        result.reason = "no_goal";
        return result;
    }

    if (request.recent_blocked_frames >= request.goal.max_no_progress_frames) {
        result.status = ::animation_update::custom_controllers::internal::MovementGoalStatus::Blocked;
        result.reason = "blocked_escalate_to_intent";
        return result;
    }

    if (!crowded(result.local_waypoint, request.occupied_positions)) {
        result.status = ::animation_update::custom_controllers::internal::MovementGoalStatus::Active;
        result.reason = "direct_waypoint";
        return result;
    }

    const std::array<axis::WorldPos, 4> offsets{{
        axis::WorldPos{32, 0, 0}, axis::WorldPos{-32, 0, 0},
        axis::WorldPos{0, 0, 32}, axis::WorldPos{0, 0, -32},
    }};
    for (const axis::WorldPos& offset : offsets) {
        axis::WorldPos candidate{result.local_waypoint.x + offset.x,
                                 result.local_waypoint.y,
                                 result.local_waypoint.z + offset.z};
        if (!crowded(candidate, request.occupied_positions)) {
            result.local_waypoint = candidate;
            result.status = ::animation_update::custom_controllers::internal::MovementGoalStatus::Active;
            result.reason = "local_avoidance_offset";
            return result;
        }
    }

    result.status = ::animation_update::custom_controllers::internal::MovementGoalStatus::Blocked;
    result.reason = "crowded_no_offset";
    return result;
}

} // namespace animation_update::navigation
