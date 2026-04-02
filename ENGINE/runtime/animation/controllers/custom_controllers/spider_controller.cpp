#include "spider_controller.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/range_util.hpp"
#include <iostream>
#include <optional>

spider_controller::spider_controller(Asset* self)
    : CustomAssetController(self) {
        std::cout<<"Spider Controller Connected";
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(true);
        owner->needs_target = true;
    }
}

void spider_controller::on_update(const Input&) {
    constexpr int kSpiderStopRadiusPx = 96;

    Asset* self = self_ptr();
    Assets* assets = this->assets();
    if (!self || !self->anim_ || !assets) {
        return;
    }
    Asset* player = assets->player;

    if (!player || player == self || player->dead || !player->active) {
        return;
    }

    const bool in_attack_range = Range::is_in_range(self, player, kSpiderStopRadiusPx);
    if (in_attack_range) {
        if (!self->needs_target) {
            self->anim_->cancel_all_movement();
        }
    } else if (self->needs_target) {
        self->anim_->auto_move(player);
    }

    animation_update::custom_controllers::AttackDetectionHelper::send_attack_if_hit(self, player);
}

void spider_controller::on_process_pending_attacks(Asset& self) {
    const auto pending_attacks = self.process_pending_attacks();
    if (pending_attacks.empty()) {
        return;
    }

    std::optional<SDL_Point> bump_delta;
    auto consider_knockback = [&](const animation_update::Attack& attack) {
        SDL_Point candidate_delta{};
        if (!animation_update::custom_controllers::AttackProcessingHelper::compute_knockback_delta(self, attack, candidate_delta)) {
            return;
        }
        if (!bump_delta.has_value()) {
            bump_delta = candidate_delta;
            return;
        }
        const int candidate_sq = candidate_delta.x * candidate_delta.x + candidate_delta.y * candidate_delta.y;
        const int current_sq = bump_delta->x * bump_delta->x + bump_delta->y * bump_delta->y;
        if (candidate_sq > current_sq) {
            bump_delta = candidate_delta;
        }
    };

    for (const auto& attack : pending_attacks) {
        self.runtime_health -= attack.damage_amount;
        if (attack.attacker_asset_name == "vibble") {
            consider_knockback(attack);
        }
    }

    if (self.runtime_health < 0) {
        if (!animation_update::custom_controllers::AttackProcessingHelper::try_play_death_animation(self)) {
            self.Delete();
        }
        return;
    }

    if (bump_delta.has_value()) {
        animation_update::custom_controllers::AttackProcessingHelper::apply_knockback(self, *bump_delta);
    }
}
