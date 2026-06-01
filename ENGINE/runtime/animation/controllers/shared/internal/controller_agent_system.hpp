#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "animation/controllers/shared/controller_types.hpp"
#include "animation/controllers/shared/internal/enemy_movement_goal.hpp"
#include "core/axis_convention.hpp"

class Asset;

namespace animation_update::custom_controllers::internal {

using EnemyAgentPhase = ::animation_update::custom_controllers::EnemyAgentPhase;
using EnemyAgentConfig = ::animation_update::custom_controllers::EnemyAgentConfig;
using MovementConfig = ::animation_update::custom_controllers::MovementConfig;

struct PatrolState {
    std::size_t next_index = 0;
};

struct BehaviorState {
    EnemyAgentPhase mode = EnemyAgentPhase::Idle;
    PatrolState patrol_state{};
    std::chrono::steady_clock::time_point recover_until{};
    std::chrono::steady_clock::time_point attack_window_until{};
    axis::WorldPos home{0, 0, 0};
    bool initialized_home = false;
    int no_progress_frames = 0;
    int return_home_fallback_count = 0;
    int attack_window_enter_count = 0;
    int attack_window_exit_count = 0;
    MovementGoal active_movement_goal{};
    MovementGoalResult last_movement_result{};
    int movement_goal_change_count = 0;
    std::string last_movement_goal_reason{};
};

class ControllerAgentSystem {
public:
    struct EnemyPhaseDecision {
        EnemyAgentPhase phase = EnemyAgentPhase::Idle;
        bool target_should_be_committed = false;
        bool enter_attack_window = false;
        bool leave_attack_window_to_recover = false;
    };

    static EnemyPhaseDecision decide_enemy_phase(const BehaviorState& state,
                                                 bool target_valid,
                                                 long long target_dist_sq,
                                                 std::chrono::steady_clock::time_point now,
                                                 const EnemyAgentConfig& config) {
        EnemyPhaseDecision decision{};
        if (!target_valid) {
            decision.phase = EnemyAgentPhase::ReturnHome;
            return decision;
        }

        const int aggro_radius_px = std::max(0, config.ranges.aggro_radius_px);
        const int attack_radius_px = std::max(0, config.ranges.attack_radius_px);
        const long long aggro_radius_sq =
            static_cast<long long>(aggro_radius_px) * static_cast<long long>(aggro_radius_px);
        const long long attack_radius_sq =
            static_cast<long long>(attack_radius_px) * static_cast<long long>(attack_radius_px);

        if (target_dist_sq > aggro_radius_sq) {
            decision.phase = EnemyAgentPhase::ReturnHome;
            return decision;
        }

        if (state.mode == EnemyAgentPhase::Recover && now < state.recover_until) {
            decision.phase = EnemyAgentPhase::Recover;
            return decision;
        }

        if (target_dist_sq <= attack_radius_sq) {
            decision.phase = EnemyAgentPhase::AttackWindow;
            decision.target_should_be_committed = true;
            if (state.mode != EnemyAgentPhase::AttackWindow) {
                decision.enter_attack_window = true;
            } else if (now >= state.attack_window_until && !config.kamikaze) {
                decision.phase = EnemyAgentPhase::Recover;
                decision.leave_attack_window_to_recover = true;
                decision.target_should_be_committed = false;
            }
            return decision;
        }

        decision.phase = EnemyAgentPhase::Approach;
        return decision;
    }

    static bool move_by_delta_3d(Asset& self,
                                 const axis::WorldPos& delta,
                                 const std::string& animation,
                                 bool override_non_locked = true);
    static bool move_toward_point(Asset& self,
                                  const axis::WorldPos& target,
                                  int step_px,
                                  const MovementConfig& config = {});
    static bool move_away_from_point(Asset& self,
                                     const axis::WorldPos& point,
                                     int step_px,
                                     const MovementConfig& config = {});
    static bool seek_target(Asset& self,
                            const Asset& target,
                            int desired_range_px,
                            const MovementConfig& config = {});
    static bool chase_target(Asset& self,
                             const Asset& target,
                             const MovementConfig& config = {});
    static bool retreat_from_target(Asset& self,
                                    const Asset& target,
                                    int retreat_distance_px,
                                    const MovementConfig& config = {});
    static bool patrol(Asset& self,
                       const std::vector<axis::WorldPos>& points,
                       PatrolState& patrol_state,
                       const MovementConfig& config = {});
    static bool idle_wander(Asset& self,
                            std::mt19937& rng,
                            int min_delta_px,
                            int max_delta_px,
                            const MovementConfig& config = {});
    static bool return_to_origin(Asset& self,
                                 const axis::WorldPos& origin,
                                 int arrival_threshold_px,
                                 const MovementConfig& config = {});
    static bool face_target(Asset& self, const Asset& target);
    static bool face_direction(Asset& self, float dir_x, float dir_z, float pitch_radians = 0.0f);

    static bool tick_wander(Asset& self,
                            Asset* target,
                            BehaviorState& state,
                            std::mt19937& rng,
                            int idle_radius_px,
                            int min_wander_delta_px,
                            int max_wander_delta_px,
                            const MovementConfig& config = {});
    static void tick_enemy_behavior(Asset& self,
                                    Asset* target,
                                    BehaviorState& state,
                                    const EnemyAgentConfig& config,
                                    const MovementConfig& chase_move = {},
                                    const MovementConfig& retreat_move = {});
    static bool tick_patrol(Asset& self,
                            BehaviorState& state,
                            const std::vector<axis::WorldPos>& patrol_points,
                            const MovementConfig& config = {});
    static bool tick_return_home(Asset& self,
                                 BehaviorState& state,
                                 int arrival_threshold_px,
                                 const MovementConfig& config = {});

    static long long distance_sq_3d(const Asset& a, const Asset& b);
    static long long distance_sq_3d(const Asset& a, const axis::WorldPos& b);

private:
    static void ensure_home(BehaviorState& state, const Asset& self);
    static axis::WorldPos world_position(const Asset& self);
    static bool auto_move_3d(Asset& self,
                             const axis::WorldPos& target,
                             const MovementConfig& config);
};

} // namespace animation_update::custom_controllers::internal
