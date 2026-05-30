#pragma once

#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

#include "animation/controllers/shared/controller_types.hpp"
#include "animation/controllers/shared/internal/controller_movement_system.hpp"
#include "core/axis_convention.hpp"

class Asset;

namespace animation_update::custom_controllers::internal {

using EnemyAgentPhase = ::animation_update::custom_controllers::EnemyAgentPhase;
using EnemyAgentConfig = ::animation_update::custom_controllers::EnemyAgentConfig;

struct BehaviorState {
    EnemyAgentPhase mode = EnemyAgentPhase::Idle;
    PatrolState patrol_state{};
    std::chrono::steady_clock::time_point recover_until{};
    std::chrono::steady_clock::time_point attack_window_until{};
    axis::WorldPos home{0, 0, 0};
    bool initialized_home = false;
    int no_progress_frames = 0;
};

class ControllerBehaviorSystem {
public:
    struct EnemyPhaseDecision {
        EnemyAgentPhase phase = EnemyAgentPhase::Idle;
        bool target_should_be_committed = false;
        bool enter_attack_window = false;
        bool leave_attack_window_to_recover = false;
    };

    static void ensure_home(BehaviorState& state, const Asset& self);
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
};

} // namespace animation_update::custom_controllers::internal
