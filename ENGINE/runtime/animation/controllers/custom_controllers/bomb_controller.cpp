#include "bomb_controller.hpp"
#include "animation/controllers/shared/custom_controller_update_utils.hpp"
#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include <iostream>
#include "animation/controllers/shared/attack_processing_helper.hpp"
namespace animation_update::custom_controllers {}
namespace custom_controllers = animation_update::custom_controllers;

bomb_controller::bomb_controller(Asset* self)
    : CustomAssetController(self) {
        std::cout<<"bomb Controller Connected";
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void bomb_controller::on_update(const Input& in) {
    const auto& ctx = game_context();
    Asset* self = self_ptr();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }
    Asset* player = animation_update::custom_controllers::resolve_valid_player_target(ctx);

    if (!player) {
        return;
    }

    if (self->needs_target) {
        self->anim_->auto_move_3d(player, 0, false);
    }
    CustomAssetController::on_update(in);
}

void bomb_controller::on_process_pending_attacks(Asset& self) {
    custom_controllers::AttackProcessingHelper::process_pending_attacks(self);
}
