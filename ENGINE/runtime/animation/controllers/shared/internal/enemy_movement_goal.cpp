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

MovementGoalResult evaluate_movement_goal(const MovementGoal& goal,
                                          const axis::WorldPos& current_position,
                                          int no_progress_frames) {
    MovementGoalResult result{};
    result.current_position = current_position;
    result.final_destination = goal.target_position;
    result.no_progress_frames = std::max(0, no_progress_frames);
    if (goal.kind == MovementGoalKind::None) {
        result.status = MovementGoalStatus::None;
        result.reason = "no_goal";
        return result;
    }

    const long long dx = static_cast<long long>(goal.target_position.x) - current_position.x;
    const long long dy = goal.allow_vertical_movement
                             ? static_cast<long long>(goal.target_position.y) - current_position.y
                             : 0;
    const long long dz = static_cast<long long>(goal.target_position.z) - current_position.z;
    const long long dist_sq = (dx * dx) + (dy * dy) + (dz * dz);
    const int reached = std::max(0, goal.desired_range_px + goal.tolerance_px);
    if (dist_sq <= static_cast<long long>(reached) * reached) {
        result.status = MovementGoalStatus::Reached;
        result.reason = "inside_goal_band";
        return result;
    }
    if (result.no_progress_frames >= std::max(1, goal.max_no_progress_frames)) {
        result.status = MovementGoalStatus::Blocked;
        result.reason = "no_progress_threshold";
        return result;
    }
    result.status = MovementGoalStatus::Active;
    result.reason = "progressing";
    return result;
}

bool should_reuse_movement_goal(const MovementGoal& active_goal,
                                const MovementGoal& requested_goal) {
    return active_goal.kind != MovementGoalKind::None && materially_same_goal(active_goal, requested_goal);
}

} // namespace animation_update::custom_controllers::internal
