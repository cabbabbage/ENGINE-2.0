#pragma once

#include <optional>

#include "animation/animation_update.hpp"

namespace animation_update::custom_controllers {

struct MovementConfig {
    int visit_threshold_px = 0;
    std::optional<int> resolution_layer = std::nullopt;
    bool override_non_locked = true;
    bool allow_vertical_movement = false;
    AnimationUpdate::AutoMoveCombatOverrides combat_overrides{};
};

enum class EnemyAgentPhase {
    Idle,
    Acquire,
    Approach,
    AttackWindow,
    Recover,
    ReturnHome,
    Patrol,
    Custom,
};

struct EnemyRangeConfig {
    // Detection radius for engaging a target.
    int aggro_radius_px = 260;
    // Preferred stand-off distance while approaching (0 means close to contact).
    int desired_standoff_px = 0;
    // Distance considered valid for attack window entry.
    int attack_radius_px = 80;
};

struct EnemyAgentConfig {
    EnemyRangeConfig ranges{};
    int retreat_distance_px = 240;
    int return_home_threshold_px = 48;
    int recover_ms = 500;
    int attack_window_ms = 140;
    bool kamikaze = false;
    bool force_attacking_enabled = false;
    bool require_ground_contact = true;
    int airborne_buffer_px = 1;
};

} // namespace animation_update::custom_controllers
