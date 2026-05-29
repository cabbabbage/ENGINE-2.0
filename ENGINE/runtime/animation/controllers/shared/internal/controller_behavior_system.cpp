#include "animation/controllers/shared/internal/controller_behavior_system.hpp"

#include <algorithm>
#include <cmath>

#include "assets/asset/Asset.hpp"

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
        state.mode = BehaviorMode::Idle;
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
        state.mode = BehaviorMode::Idle;
        return false;
    }

    state.mode = BehaviorMode::Seek;
    return ControllerMovementSystem::idle_wander(self,
                                                 rng,
                                                 min_wander_delta_px,
                                                 max_wander_delta_px,
                                                 config);
}

void ControllerBehaviorSystem::tick_enemy_behavior(Asset& self,
                                                   Asset* target,
                                                   BehaviorState& state,
                                                   const EnemyBehaviorConfig& config,
                                                   const MovementConfig& chase_move,
                                                   const MovementConfig& retreat_move) {
    ensure_home(state, self);

    if (!target || target->dead || !target->active) {
        state.mode = BehaviorMode::Return;
        (void)tick_return_home(self, state, config.return_home_threshold_px, chase_move);
        return;
    }

    const long long attack_range_sq =
        static_cast<long long>(std::max(0, config.attack_range_px)) *
        static_cast<long long>(std::max(0, config.attack_range_px));
    const long long target_dist_sq = distance_sq_3d(self, *target);

    if (config.kamikaze) {
        state.mode = BehaviorMode::Chase;
        if (target_dist_sq <= attack_range_sq) {
            state.mode = BehaviorMode::Attack;
            self.needs_target = false;
            self.target_reached = true;
            return;
        }

        MovementConfig chase_cfg = chase_move;
        chase_cfg.combat_overrides.attacking_enabled = true;
        chase_cfg.combat_overrides.force_attacking_enabled = config.force_attacking_enabled;
        (void)ControllerMovementSystem::seek_target(
            self,
            *target,
            std::max(0, config.chase_range_px),
            chase_cfg);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (state.mode == BehaviorMode::Recover) {
        if (now < state.recover_until) {
            (void)ControllerMovementSystem::retreat_from_target(
                self,
                *target,
                std::max(1, config.retreat_distance_px),
                retreat_move);
            return;
        }
        state.mode = BehaviorMode::Chase;
    }

    if (target_dist_sq <= attack_range_sq) {
        state.mode = BehaviorMode::Recover;
        state.recover_until = now + std::chrono::milliseconds(std::max(1, config.recover_ms));
        return;
    }

    state.mode = BehaviorMode::Chase;
    (void)ControllerMovementSystem::seek_target(
        self,
        *target,
        std::max(0, config.chase_range_px),
        chase_move);
}

bool ControllerBehaviorSystem::tick_patrol(Asset& self,
                                           BehaviorState& state,
                                           const std::vector<axis::WorldPos>& patrol_points,
                                           const MovementConfig& config) {
    ensure_home(state, self);
    if (patrol_points.empty()) {
        return false;
    }
    state.mode = BehaviorMode::Patrol;
    return ControllerMovementSystem::patrol(self, patrol_points, state.patrol_state, config);
}

bool ControllerBehaviorSystem::tick_return_home(Asset& self,
                                                BehaviorState& state,
                                                int arrival_threshold_px,
                                                const MovementConfig& config) {
    ensure_home(state, self);
    state.mode = BehaviorMode::Return;
    return ControllerMovementSystem::return_to_origin(self,
                                                      state.home,
                                                      std::max(0, arrival_threshold_px),
                                                      config);
}

} // namespace animation_update::custom_controllers::internal
