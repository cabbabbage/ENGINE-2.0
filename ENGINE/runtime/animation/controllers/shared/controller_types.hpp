#pragma once

#include <optional>
#include <string_view>
#include <vector>

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

struct AttackProcessingConfig {
    float max_knockback_distance = 50.0f;
    int max_damage_for_knockback = 100;
    float knockback_scale = 1.0f;
    std::string_view hit_animation_id = "hit";
    std::string_view death_animation_id = "die";
    std::string_view hit_fallback_animation_id = "default";
    std::string_view death_fallback_tag = "break";
};

struct AttackProcessingSummary {
    bool had_pending_attacks = false;
    bool took_damage = false;
    bool died = false;
};

} // namespace animation_update::custom_controllers
