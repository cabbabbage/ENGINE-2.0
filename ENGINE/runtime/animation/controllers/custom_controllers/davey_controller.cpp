#include "davey_controller.hpp"
#include "animation/controllers/shared/custom_controller_update_utils.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

davey_controller::davey_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void davey_controller::on_update(const Input&) {
    Asset* self = self_ptr();
    Assets* assets = this->assets();
    if (!self || !self->anim_ || !assets) {
        return;
    }

    Asset* player = animation_update::custom_controllers::resolve_valid_player_target(self, assets);
    if (!player) {
        return;
    }

    self->anim_->auto_move(player);

    animation_update::custom_controllers::dispatch_contact_attack(self, player);
}

void davey_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
