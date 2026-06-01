#include <cassert>
#include <chrono>
#include <string>

#include "animation/controllers/shared/internal/enemy_movement_goal.hpp"
#include "animation/controllers/shared/internal/enemy_positioning_system.hpp"
#include "animation/navigation/enemy_navigation_system.hpp"

int main() {
    using namespace animation_update::custom_controllers::internal;

    {
        MovementGoal goal{};
        goal.kind = MovementGoalKind::MaintainRange;
        goal.target_position = axis::WorldPos{100, 0, 0};
        goal.desired_range_px = 20;
        goal.tolerance_px = 4;
        goal.max_no_progress_frames = 3;
        assert(evaluate_movement_goal(goal, axis::WorldPos{80, 0, 0}, 0).status == MovementGoalStatus::Reached);
        assert(evaluate_movement_goal(goal, axis::WorldPos{0, 0, 0}, 4).status == MovementGoalStatus::Blocked);

        MovementGoal shifted = goal;
        shifted.target_position.x += 8;
        assert(should_reuse_movement_goal(goal, shifted));
        shifted.target_position.x += 128;
        assert(!should_reuse_movement_goal(goal, shifted));
    }

    {
        CombatSlotTable slots{};
        slots.active_attacker_limit = 1;
        const auto now = std::chrono::steady_clock::now();
        const axis::WorldPos target{0, 0, 0};
        const auto candidates = EnemyPositioningSystem::scored_candidates(axis::WorldPos{100, 0, 0},
                                                                          target,
                                                                          48,
                                                                          slots,
                                                                          "player",
                                                                          now);
        auto reservation = EnemyPositioningSystem::reserve_best_slot(slots,
                                                                     "enemy_a",
                                                                     "player",
                                                                     candidates,
                                                                     now,
                                                                     std::chrono::milliseconds(200));
        assert(reservation.has_value());
        assert(EnemyPositioningSystem::active_reservation_count(slots, "player", now) == 1);
        auto denied = EnemyPositioningSystem::reserve_best_slot(slots,
                                                               "enemy_b",
                                                               "player",
                                                               candidates,
                                                               now,
                                                               std::chrono::milliseconds(200));
        assert(!denied.has_value());
        EnemyPositioningSystem::prune_expired(slots, now + std::chrono::milliseconds(201));
        assert(EnemyPositioningSystem::active_reservation_count(slots,
                                                               "player",
                                                               now + std::chrono::milliseconds(201)) == 0);
    }

    {
        animation_update::navigation::EnemyNavigationRequest request{};
        request.goal.kind = MovementGoalKind::MoveToPoint;
        request.goal.target_position = axis::WorldPos{64, 0, 0};
        request.goal.max_no_progress_frames = 2;
        request.occupied_positions.push_back(axis::WorldPos{64, 0, 0});
        auto result = animation_update::navigation::EnemyNavigationSystem::choose_local_waypoint(request);
        assert(result.status == MovementGoalStatus::Active);
        assert(result.reason == "local_avoidance_offset");
        request.recent_blocked_frames = 2;
        result = animation_update::navigation::EnemyNavigationSystem::choose_local_waypoint(request);
        assert(result.status == MovementGoalStatus::Blocked);
    }
}
