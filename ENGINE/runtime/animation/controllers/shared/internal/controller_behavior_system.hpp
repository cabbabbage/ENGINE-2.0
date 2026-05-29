#pragma once

#include <chrono>
#include <random>
#include <vector>

#include "animation/controllers/shared/internal/controller_movement_system.hpp"
#include "core/axis_convention.hpp"

class Asset;

namespace animation_update::custom_controllers::internal {

enum class BehaviorMode {
    Idle,
    Patrol,
    Seek,
    Chase,
    Attack,
    Recover,
    Return,
    Custom,
};

struct BehaviorState {
    BehaviorMode mode = BehaviorMode::Idle;
    PatrolState patrol_state{};
    std::chrono::steady_clock::time_point recover_until{};
    axis::WorldPos home{0, 0, 0};
    bool initialized_home = false;
};

struct EnemyBehaviorConfig {
    int chase_range_px = 220;
    int attack_range_px = 80;
    int retreat_distance_px = 240;
    int return_home_threshold_px = 48;
    int recover_ms = 500;
    bool kamikaze = false;
    bool force_attacking_enabled = false;
};

class ControllerBehaviorSystem {
public:
    static void ensure_home(BehaviorState& state, const Asset& self);

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
                                    const EnemyBehaviorConfig& config,
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
