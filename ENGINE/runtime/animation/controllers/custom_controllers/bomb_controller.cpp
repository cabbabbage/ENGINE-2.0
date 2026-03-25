#include "bomb_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

bomb_controller::bomb_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void bomb_controller::on_update(const Input&) {
    Asset* self = self_ptr();
    Assets* assets = this->assets();
    if (!self || !self->anim_ || !assets) {
        return;
    }
    Asset* player = assets->player;

    if (!player || player == self || player->dead || !player->active) {
        return;
    }

    int distance_sq = (self->world_x() - player->world_x()) * (self->world_x() - player->world_x()) + (self->world_z() - player->world_z()) * (self->world_z() - player->world_z());

    if (distance_sq <= 700) {
        if (self->info && self->info->animations.count("explosion")) {
            self->anim_->set_animation("explosion");
        }
    } else if (self->needs_target) {
        self->anim_->auto_move(player);
    }

    attack_helpers::send_attack_if_hit(self, player);
}

void bomb_controller::on_process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
