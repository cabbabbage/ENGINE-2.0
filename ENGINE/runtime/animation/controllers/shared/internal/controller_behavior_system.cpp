#include "animation/controllers/shared/internal/controller_behavior_system.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "assets/asset/Asset.hpp"
#include "utils/log.hpp"

namespace animation_update::custom_controllers::internal {

namespace {

long long distance_sq_3d(const Asset& a, const Asset& b) {
    return ControllerMovementSystem::distance_sq_3d(a, b);
}

long long distance_sq_3d(const Asset& a, const axis::WorldPos& b) {
    return ControllerMovementSystem::distance_sq_3d(a, b);
}

} // namespace

void ControllerBehaviorSystem::ensure_home(BehaviorState& state, const Asset& self) {
    if (state.initialized_home) {
        return;
    }
    state.home = axis::WorldPos{self.world_x(), self.world_y(), self.world_z()};
    state.initialized_home = true;
}

bool ControllerBehaviorSystem::tick_wander(Asset& self,
                                           Asset* target,
                                           BehaviorState& state,
                                           std::mt19937& rng,
                                           int idle_radius_px,
                                           int min_wander_delta_px,
                                           int max_wander_delta_px,
                                           const MovementConfig& config) {
    ensure_home(state, self);

    if (!target) {
        state.mode = EnemyAgentPhase::Idle;
        return ControllerMovementSystem::idle_wander(self,
                                                     rng,
                                                     min_wander_delta_px,
                                                     max_wander_delta_px,
                                                     config);
    }

    const long long dist_sq = distance_sq_3d(self, *target);
    const long long idle_sq = static_cast<long long>(std::max(0, idle_radius_px)) *
                              static_cast<long long>(std::max(0, idle_radius_px));
    if (dist_sq <= idle_sq) {
        state.mode = EnemyAgentPhase::Idle;
        return false;
    }

    state.mode = EnemyAgentPhase::Acquire;
    return ControllerMovementSystem::idle_wander(self,
                                                 rng,
                                                 min_wander_delta_px,
                                                 max_wander_delta_px,
                                                 config);
}

void ControllerBehaviorSystem::tick_enemy_behavior(Asset& self,
                                                   Asset* target,
                                                   BehaviorState& state,
                                                   const EnemyAgentConfig& config,
                                                   const MovementConfig& chase_move,
                                                   const MovementConfig& retreat_move) {
    ensure_home(state, self);
    const bool target_valid = target && !target->dead && target->active;
    const int desired_standoff_px = std::max(0, config.ranges.desired_standoff_px);
    const long long target_dist_sq = target_valid ? distance_sq_3d(self, *target) : 0;
    const auto now = std::chrono::steady_clock::now();
    const EnemyPhaseDecision decision =
        decide_enemy_phase(state, target_valid, target_dist_sq, now, config);
    bool moved = false;

    MovementConfig chase_cfg = chase_move;
    chase_cfg.combat_overrides.attacking_enabled = true;
    chase_cfg.combat_overrides.force_attacking_enabled = config.force_attacking_enabled;
    MovementConfig retreat_cfg = retreat_move;
    retreat_cfg.combat_overrides.attacking_enabled = false;

    state.mode = decision.phase;
    if (decision.target_should_be_committed) {
        self.needs_target = false;
        self.target_reached = true;
    } else {
        self.target_reached = false;
        self.needs_target = true;
    }

    if (decision.phase == EnemyAgentPhase::ReturnHome) {
        moved = tick_return_home(self, state, config.return_home_threshold_px, chase_cfg);
        state.no_progress_frames = moved ? 0 : state.no_progress_frames + 1;
        return;
    }

    if (!target_valid) {
        return;
    }

    if (decision.enter_attack_window) {
        state.attack_window_until =
            now + std::chrono::milliseconds(std::max(1, config.attack_window_ms));
    }
    if (decision.leave_attack_window_to_recover) {
        state.recover_until = now + std::chrono::milliseconds(std::max(1, config.recover_ms));
    }

    if (decision.phase == EnemyAgentPhase::Recover) {
        moved = ControllerMovementSystem::retreat_from_target(
            self,
            *target,
            std::max(1, config.retreat_distance_px),
            retreat_cfg);
        state.no_progress_frames = moved ? 0 : state.no_progress_frames + 1;
        return;
    }
    if (decision.phase == EnemyAgentPhase::Approach) {
        moved = ControllerMovementSystem::seek_target(self, *target, desired_standoff_px, chase_cfg);
        if (!moved && target_dist_sq > static_cast<long long>(config.ranges.attack_radius_px) *
                                           static_cast<long long>(config.ranges.attack_radius_px)) {
            ++state.no_progress_frames;
        } else {
            state.no_progress_frames = 0;
        }
        if (state.no_progress_frames >= 45) {
            const std::string self_name =
                (self.info && !self.info->name.empty()) ? self.info->name : std::string{"<unknown>"};
            vibble::log::warn("[EnemyAI] No progress while approaching target; forcing return-home fallback for '" +
                              self_name + "'");
            state.mode = EnemyAgentPhase::ReturnHome;
            (void)tick_return_home(self, state, config.return_home_threshold_px, chase_cfg);
            state.no_progress_frames = 0;
        }
        return;
    }

    state.no_progress_frames = 0;
}

bool ControllerBehaviorSystem::tick_patrol(Asset& self,
                                           BehaviorState& state,
                                           const std::vector<axis::WorldPos>& patrol_points,
                                           const MovementConfig& config) {
    ensure_home(state, self);
    if (patrol_points.empty()) {
        return false;
    }
    state.mode = EnemyAgentPhase::Patrol;
    return ControllerMovementSystem::patrol(self, patrol_points, state.patrol_state, config);
}

bool ControllerBehaviorSystem::tick_return_home(Asset& self,
                                                BehaviorState& state,
                                                int arrival_threshold_px,
                                                const MovementConfig& config) {
    ensure_home(state, self);
    state.mode = EnemyAgentPhase::ReturnHome;
    return ControllerMovementSystem::return_to_origin(self,
                                                      state.home,
                                                      std::max(0, arrival_threshold_px),
                                                      config);
}

} // namespace animation_update::custom_controllers::internal
