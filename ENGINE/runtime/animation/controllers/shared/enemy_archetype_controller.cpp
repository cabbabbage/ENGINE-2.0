#include "animation/controllers/shared/enemy_archetype_controller.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace animation_update::custom_controllers::enemy_archetypes {

MeleeChaserPreset EnemyArchetypePresets::spider() {
    MeleeChaserPreset preset{};
    preset.behavior.kamikaze = false;
    preset.behavior.ranges.aggro_radius_px = 230;
    preset.behavior.ranges.desired_standoff_px = 8;
    preset.behavior.ranges.attack_radius_px = 72;
    preset.behavior.retreat_distance_px = 310;
    preset.behavior.recover_ms = 520;
    preset.behavior.attack_window_ms = 220;
    preset.behavior.return_home_threshold_px = 130;
    preset.behavior.force_attacking_enabled = true;
    preset.approach_move.visit_threshold_px = 12;
    preset.approach_move.allow_vertical_movement = false;
    preset.retreat_move.visit_threshold_px = 12;
    preset.retreat_move.allow_vertical_movement = false;
    preset.primary_attack = internal::EnemyCombatCoordinator::make_legacy_profile(
        "spider_primary", 0.55f, preset.behavior.ranges.attack_radius_px + 24, "attack_left");
    return preset;
}

MeleeChaserPreset EnemyArchetypePresets::boneski() {
    MeleeChaserPreset preset{};
    preset.behavior.kamikaze = false;
    preset.behavior.ranges.aggro_radius_px = 240;
    preset.behavior.ranges.desired_standoff_px = 10;
    preset.behavior.ranges.attack_radius_px = 74;
    preset.behavior.retreat_distance_px = 220;
    preset.behavior.recover_ms = 430;
    preset.behavior.attack_window_ms = 220;
    preset.behavior.return_home_threshold_px = 110;
    preset.behavior.force_attacking_enabled = true;
    preset.approach_move.visit_threshold_px = 12;
    preset.retreat_move.visit_threshold_px = 12;
    preset.primary_attack = internal::EnemyCombatCoordinator::make_legacy_profile(
        "boneski_primary", 0.7f, preset.behavior.ranges.attack_radius_px + 24, "attack_right");
    return preset;
}

MeleeChaserPreset EnemyArchetypePresets::small_spider() {
    MeleeChaserPreset preset{};
    preset.behavior.kamikaze = false;
    preset.behavior.ranges.aggro_radius_px = 120;
    preset.behavior.ranges.desired_standoff_px = 3;
    preset.behavior.ranges.attack_radius_px = 16;
    preset.behavior.retreat_distance_px = 140;
    preset.behavior.recover_ms = 210;
    preset.behavior.attack_window_ms = 160;
    preset.behavior.return_home_threshold_px = 70;
    preset.behavior.force_attacking_enabled = true;
    preset.behavior.airborne_buffer_px = 1;
    preset.approach_move.visit_threshold_px = 10;
    preset.approach_move.override_non_locked = false;
    preset.retreat_move.visit_threshold_px = 10;
    preset.retreat_move.override_non_locked = false;
    preset.primary_attack = internal::EnemyCombatCoordinator::make_legacy_profile(
        "small_spider_primary", 0.35f, preset.behavior.ranges.attack_radius_px + 8, "attack");
    return preset;
}

ExploderPreset EnemyArchetypePresets::bomb() {
    ExploderPreset preset{};
    preset.trigger_radius_px = 42;
    preset.explosion_radius_px = 130;
    preset.arming_frames = 8;
    preset.active_frames = 2;
    preset.behavior.kamikaze = true;
    preset.behavior.ranges.aggro_radius_px = 260;
    preset.behavior.ranges.attack_radius_px = preset.trigger_radius_px;
    preset.behavior.attack_window_ms = 200;
    preset.behavior.force_attacking_enabled = true;
    preset.explosion_attack = internal::EnemyCombatCoordinator::make_explosion_profile(
        "bomb_explosion", preset.arming_frames, preset.active_frames, preset.explosion_radius_px, 70);
    return preset;
}

ContactHazardPreset EnemyArchetypePresets::aggressive_fly() {
    ContactHazardPreset preset{};
    preset.cooldown_frames = 18;
    preset.contact_attack = internal::EnemyCombatCoordinator::make_contact_hazard_profile(
        "fly_contact", preset.cooldown_frames, 24);
    return preset;
}

SkittishCritterPreset EnemyArchetypePresets::frog() {
    return SkittishCritterPreset{96, 220, 24, 72};
}

axis::WorldPos SkittishCritterEnemy::choose_safe_position(const axis::WorldPos& self_position,
                                                          const axis::WorldPos& threat_position,
                                                          const SkittishCritterPreset& preset,
                                                          std::mt19937& rng) {
    std::uniform_real_distribution<double> noise(-0.35, 0.35);
    std::uniform_int_distribution<int> step_dist(std::max(1, preset.min_step_px),
                                                 std::max(std::max(1, preset.min_step_px), preset.max_step_px));
    double vx = static_cast<double>(self_position.x - threat_position.x);
    double vz = static_cast<double>(self_position.z - threat_position.z);
    if ((vx * vx) + (vz * vz) < 0.001) {
        std::uniform_real_distribution<double> dir(-1.0, 1.0);
        vx = dir(rng);
        vz = dir(rng);
    }
    double len = std::sqrt(std::max(0.0001, (vx * vx) + (vz * vz)));
    vx /= len;
    vz /= len;
    vx += noise(rng);
    vz += noise(rng);
    len = std::sqrt(std::max(0.0001, (vx * vx) + (vz * vz)));
    vx /= len;
    vz /= len;
    const int min_step_for_safety = std::max(preset.min_step_px, preset.safe_distance_px / 4);
    const int sampled = step_dist(rng);
    const int step = std::max(min_step_for_safety, sampled);
    return axis::WorldPos{self_position.x + static_cast<int>(std::lround(vx * step)),
                          self_position.y,
                          self_position.z + static_cast<int>(std::lround(vz * step))};
}

} // namespace animation_update::custom_controllers::enemy_archetypes
