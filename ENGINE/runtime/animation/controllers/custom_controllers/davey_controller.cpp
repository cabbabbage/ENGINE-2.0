#include "davey_controller.hpp"
#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"

davey_controller::davey_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    Asset* owner = controller_self();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void davey_controller::on_update(const Input&) {
    const auto& ctx = controller_game_context();
    Asset* self = controller_self();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = resolve_target_player();
    if (!player) {
        return;
    }

    chase_target(*player);
    (void)try_attack_target(*player, "davey_primary", 0.8f, 72, "attack");
}

void davey_controller::on_process_pending_attacks(Asset& self) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self);
}
