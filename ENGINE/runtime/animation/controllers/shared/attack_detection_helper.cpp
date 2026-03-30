#include "attack_detection_helper.hpp"

#include "animation/attack_validation.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace animation_update::custom_controllers {

namespace {

bool can_send_attacks_from(const Asset* attacker) {
    return attacker &&
           attacker->info &&
           attacker->current_animation_frame() &&
           !attacker->dead &&
           attacker->active &&
           !attacker->current_attack_box_volumes().empty();
}

bool can_receive_attacks_from(const Asset* attacker, const Asset* target) {
    if (!attacker || !target || attacker == target) {
        return false;
    }

    if (!target->info || !target->current_animation_frame() || target->dead || !target->active) {
        return false;
    }

    if (target->current_hit_box_volumes().empty()) {
        return false;
    }

    // Attached children should never damage their owning parent.
    if (target->has_child(attacker)) {
        return false;
    }

    return true;
}

} // namespace

void AttackDetectionHelper::send_attack_if_hit(Asset* attacker, Asset* target) {
    if (!can_send_attacks_from(attacker) || !can_receive_attacks_from(attacker, target)) {
        return;
    }

    const auto attack_opt = AttackValidation::compute_attack_if_hit(*attacker, *target);
    if (attack_opt.has_value()) {
        target->send_attack(*attack_opt);
    }
}

void AttackDetectionHelper::send_attacks_to_active_targets(Asset* attacker, Assets* assets) {
    if (!can_send_attacks_from(attacker) || !assets) {
        return;
    }

    const auto& active_assets = assets->getActive();
    for (Asset* target : active_assets) {
        if (!can_receive_attacks_from(attacker, target)) {
            continue;
        }

        const auto attack_opt = AttackValidation::compute_attack_if_hit(*attacker, *target);
        if (attack_opt.has_value()) {
            target->send_attack(*attack_opt);
        }
    }
}

void AttackDetectionHelper::process_pending_attacks_default(Asset* self) {
    if (!self || self->dead) {
        return;
    }

    const auto pending_attacks = self->process_pending_attacks();
    for (const auto& attack : pending_attacks) {
        self->runtime_health -= attack.damage_amount;
    }

    if (self->runtime_health < 0) {
        self->Delete();
    }
}

} // namespace animation_update::custom_controllers
