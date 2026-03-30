#include "spider_controller.hpp"
#include "animation/controllers/shared/attack_helpers.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/range_util.hpp"
#include <iostream>

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

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

    attack_helpers::send_attack_if_hit(self, player);
}

void spider_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
