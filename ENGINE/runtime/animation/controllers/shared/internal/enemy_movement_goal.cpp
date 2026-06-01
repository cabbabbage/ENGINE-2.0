#include "animation/controllers/shared/internal/enemy_movement_goal.hpp"

#include <algorithm>

namespace animation_update::custom_controllers::internal {

const char* movement_goal_kind_name(MovementGoalKind kind) {
    switch (kind) {
    case MovementGoalKind::None: return "None";
    case MovementGoalKind::MoveToPoint: return "MoveToPoint";
    case MovementGoalKind::PursueTarget: return "PursueTarget";
    case MovementGoalKind::MaintainRange: return "MaintainRange";
    case MovementGoalKind::RetreatFromTarget: return "RetreatFromTarget";
    case MovementGoalKind::ReturnHome: return "ReturnHome";
    case MovementGoalKind::OrbitPoint: return "OrbitPoint";
    }
    return "Unknown";
}

const char* movement_goal_status_name(MovementGoalStatus status) {
    switch (status) {
    case MovementGoalStatus::None: return "None";
    case MovementGoalStatus::Active: return "Active";
    case MovementGoalStatus::Reached: return "Reached";
    case MovementGoalStatus::Blocked: return "Blocked";
    case MovementGoalStatus::Stale: return "Stale";
    case MovementGoalStatus::Failed: return "Failed";
    case MovementGoalStatus::Interrupted: return "Interrupted";
    }
    return "Unknown";
}

bool materially_same_goal(const MovementGoal& lhs, const MovementGoal& rhs) {
    if (lhs.kind != rhs.kind ||
        lhs.target_id != rhs.target_id ||
        lhs.desired_range_px != rhs.desired_range_px ||
        lhs.allow_vertical_movement != rhs.allow_vertical_movement) {
        return false;
    }

    const int threshold = std::max(0, std::min(lhs.replan_distance_threshold_px,
                                               rhs.replan_distance_threshold_px));
    const long long dx = static_cast<long long>(lhs.target_position.x) -
                         static_cast<long long>(rhs.target_position.x);
    const long long dy = static_cast<long long>(lhs.target_position.y) -
                         static_cast<long long>(rhs.target_position.y);
    const long long dz = static_cast<long long>(lhs.target_position.z) -
                         static_cast<long long>(rhs.target_position.z);
    const long long threshold_sq = static_cast<long long>(threshold) * threshold;
    return (dx * dx) + (dy * dy) + (dz * dz) <= threshold_sq;
}

} // namespace animation_update::custom_controllers::internal
