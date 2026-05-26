#include "animation/controllers/shared/enemy_auto_combat_behavior.hpp"

#include <algorithm>

#include "animation/controllers/shared/enemy_combat_steering.hpp"
#include "assets/asset/Asset.hpp"

namespace animation_update::custom_controllers {

EnemyAutoCombatBehavior::EnemyAutoCombatBehavior(EnemyAutoCombatConfig config)
    : config_(config) {}

void EnemyAutoCombatBehavior::reset() {
    state_ = State::Approach;
    evade_until_ = {};
    was_in_attack_animation_ = false;
}

void EnemyAutoCombatBehavior::tick(Asset& self, Asset& target, EnemyCombatSteering& steering) {
    if (!self.anim_) {
        return;
    }

    const bool in_attack_animation = current_animation_has_tag(self, "attack");
    if (config_.mode == EnemyAutoCombatMode::KamikazeDetonate) {
        if (in_attack_animation) {
            state_ = State::Detonating;
            self.needs_target = false;
            self.target_reached = true;
            return;
        }

        AnimationUpdate::AutoMoveCombatOverrides combat_overrides;
        combat_overrides.attacking_enabled = true;
        combat_overrides.force_attacking_enabled = config_.force_attacking_enabled;
        steering.approach(self,
                          target,
                          std::max(0, config_.approach_range_px),
                          std::max(0, config_.approach_visit_threshold_px),
                          false,
                          combat_overrides);
        return;
    }

    if (in_attack_animation) {
        was_in_attack_animation_ = true;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (was_in_attack_animation_) {
        state_ = State::Evade;
        evade_until_ = now + std::chrono::milliseconds(std::max(1, config_.evade_duration_ms));
        was_in_attack_animation_ = false;
        steering.reset();
    }

    if (state_ == State::Evade) {
        if (now >= evade_until_) {
            state_ = State::Approach;
            steering.reset();
        } else {
            AnimationUpdate::AutoMoveCombatOverrides combat_overrides;
            combat_overrides.attacking_enabled = false;
            steering.evade(self,
                           target,
                           std::max(1, config_.evade_distance_px),
                           std::max(0, config_.evade_visit_threshold_px),
                           self.needs_target || self.target_reached || steering.is_stuck(),
                           combat_overrides);
            return;
        }
    }

    AnimationUpdate::AutoMoveCombatOverrides combat_overrides;
    combat_overrides.attacking_enabled = true;
    steering.approach(self,
                      target,
                      std::max(0, config_.approach_range_px),
                      std::max(0, config_.approach_visit_threshold_px),
                      false,
                      combat_overrides);
}

} // namespace animation_update::custom_controllers

