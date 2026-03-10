#include "spider_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

spider_controller::spider_controller(Assets* assets, Asset* self)
    : assets_(assets), self_(self) {
    if (self_ && self_->anim_) {
        self_->anim_->set_debug_enabled(false);
        self_->needs_target = true;
    }
}

void spider_controller::update(const Input&) {
    if (!self_ || !self_->anim_ || !assets_) {
        return;
    }
    Asset* player = assets_->player;

    if (!player || player == self_ || player->dead || !player->active) {
        return;
    }

    int distance_sq = (self_->world_x() - player->world_x()) * (self_->world_x() - player->world_x()) + (self_->world_z() - player->world_z()) * (self_->world_z() - player->world_z());

    if (distance_sq <= 700) {
        if (self_->info && self_->info->animations.count("explosion")) {
            self_->anim_->set_animation("explosion");
        }
    }
    else if (self_->needs_target) {
        self_->anim_->auto_move(player->world_xz_point());
    }

    attack_helpers::send_attack_if_hit(self_, player);
}

void spider_controller::process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
