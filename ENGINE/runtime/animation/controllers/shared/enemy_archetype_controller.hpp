#pragma once

#include <optional>
#include <random>
#include <string>

#include "animation/controllers/shared/controller_types.hpp"
#include "animation/controllers/shared/internal/enemy_combat_coordinator.hpp"
#include "animation/controllers/shared/internal/enemy_movement_goal.hpp"
#include "core/axis_convention.hpp"

namespace animation_update::custom_controllers::enemy_archetypes {

struct MeleeChaserPreset {
    EnemyAgentConfig behavior{};
    MovementConfig approach_move{};
    MovementConfig retreat_move{};
    internal::EnemyAttackProfile primary_attack{};
};

struct ExploderPreset {
    EnemyAgentConfig behavior{};
    internal::EnemyAttackProfile explosion_attack{};
    int trigger_radius_px = 0;
    int explosion_radius_px = 0;
    int arming_frames = 0;
    int active_frames = 0;
};

struct ContactHazardPreset {
    internal::EnemyAttackProfile contact_attack{};
    int cooldown_frames = 0;
};

struct SkittishCritterPreset {
    int threat_range_px = 0;
    int safe_distance_px = 0;
    int min_step_px = 0;
    int max_step_px = 0;
};

class EnemyArchetypePresets {
public:
    static MeleeChaserPreset spider();
    static MeleeChaserPreset boneski();
    static MeleeChaserPreset small_spider();
    static ExploderPreset bomb();
    static ContactHazardPreset aggressive_fly();
    static SkittishCritterPreset frog();
};

class SkittishCritterEnemy {
public:
    static axis::WorldPos choose_safe_position(const axis::WorldPos& self_position,
                                               const axis::WorldPos& threat_position,
                                               const SkittishCritterPreset& preset,
                                               std::mt19937& rng);
};

} // namespace animation_update::custom_controllers::enemy_archetypes
