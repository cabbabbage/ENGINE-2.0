#pragma once

#include <optional>
#include <string>

#include "core/axis_convention.hpp"

namespace animation_update::custom_controllers::internal {

enum class MovementGoalKind {
    None,
    MoveToPoint,
    PursueTarget,
    MaintainRange,
    RetreatFromTarget,
    ReturnHome,
    OrbitPoint,
};

enum class MovementGoalStatus {
    None,
    Active,
    Reached,
    Blocked,
    Stale,
    Failed,
    Interrupted,
};

struct MovementGoal {
    MovementGoalKind kind = MovementGoalKind::None;
    std::optional<std::string> target_id = std::nullopt;
    axis::WorldPos target_position{0, 0, 0};
    int desired_range_px = 0;
    int tolerance_px = 0;
    int replan_distance_threshold_px = 16;
    int max_no_progress_frames = 45;
    bool allow_vertical_movement = false;
};

struct MovementGoalResult {
    MovementGoalStatus status = MovementGoalStatus::None;
    axis::WorldPos current_position{0, 0, 0};
    axis::WorldPos final_destination{0, 0, 0};
    int no_progress_frames = 0;
    std::string reason;
};

const char* movement_goal_kind_name(MovementGoalKind kind);
const char* movement_goal_status_name(MovementGoalStatus status);
bool materially_same_goal(const MovementGoal& lhs, const MovementGoal& rhs);
MovementGoalResult evaluate_movement_goal(const MovementGoal& goal,
                                          const axis::WorldPos& current_position,
                                          int no_progress_frames);
bool should_reuse_movement_goal(const MovementGoal& active_goal,
                                const MovementGoal& requested_goal);

} // namespace animation_update::custom_controllers::internal
