#include "animation/navigation/enemy_navigation_system.hpp"

namespace animation_update::navigation {

EnemyNavigationResult EnemyNavigationSystem::choose_local_waypoint(
    const EnemyNavigationRequest& request) {
    EnemyNavigationResult result{};
    result.local_waypoint = request.goal.target_position;
    result.status = ::animation_update::custom_controllers::internal::MovementGoalStatus::Active;
    result.reason = "compat_direct_waypoint";
    return result;
}

} // namespace animation_update::navigation
