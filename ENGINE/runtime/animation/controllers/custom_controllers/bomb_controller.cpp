#include "bomb_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/range_util.hpp"

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
    constexpr int kExplosionRangePx = 700;

    Asset* self = self_ptr();
    Assets* assets = this->assets();
    if (!self || !self->anim_ || !assets) {
        return;
    }
    Asset* player = assets->player;

    if (!player || player == self || player->dead || !player->active) {
        return;
    }

    const bool in_explosion_range = Range::is_in_range(self, player, kExplosionRangePx);
    if (in_explosion_range) {
        if (!self->needs_target) {
            self->anim_->cancel_all_movement();
        }
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
