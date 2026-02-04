#include "Bomb_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

BombController::BombController(Assets* assets, Asset* self)
    : assets_(assets), self_(self) {
    if (self_ && self_->anim_) {
        self_->anim_->set_debug_enabled(false);
        self_->needs_target = true;
    }
}

void BombController::update(const Input&) {
    if (!self_ || !self_->anim_ || !assets_) {
        return;
    }
    Asset* player = assets_->player;

    if (!player || player == self_ || player->dead || !player->active) {
        return;
    }

    int distance_sq = (self_->world_x() - player->world_x()) * (self_->world_x() - player->world_x()) + (self_->world_y() - player->world_y()) * (self_->world_y() - player->world_y());

    if (distance_sq <= 700) {
        if (self_->info && self_->info->animations.count("explosion")) {
            self_->anim_->set_animation("explosion");
        }
    }
    else if (self_->needs_target) {
        self_->anim_->auto_move(player);
    }

    attack_helpers::send_attack_if_hit(self_, player);
}

void BombController::process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
