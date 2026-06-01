#pragma once

#include <string>

#include "animation/controllers/shared/internal/enemy_movement_goal.hpp"
#include "core/axis_convention.hpp"

namespace animation_update::navigation {

struct EnemyNavigationRequest {
    ::animation_update::custom_controllers::internal::MovementGoal goal{};
};

struct EnemyNavigationResult {
    axis::WorldPos local_waypoint{0, 0, 0};
    ::animation_update::custom_controllers::internal::MovementGoalStatus status =
        ::animation_update::custom_controllers::internal::MovementGoalStatus::None;
    std::string reason;
};

class EnemyNavigationSystem {
public:
    static EnemyNavigationResult choose_local_waypoint(const EnemyNavigationRequest& request);
};

} // namespace animation_update::navigation
