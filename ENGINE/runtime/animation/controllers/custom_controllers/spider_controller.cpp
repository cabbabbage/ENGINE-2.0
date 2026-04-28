#include "spider_controller.hpp"
#include "animation/animation_update.hpp"
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "assets/asset/Asset.hpp"

spider_controller::spider_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void spider_controller::on_update(const Input& in) {
    const auto& ctx = game_context();
    Asset* self = self_ptr();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }
    Asset* player = custom_controller_api::resolve_valid_player_target(ctx);

    if (!player) {
        return;
    }

    if (self->needs_target) {
        self->anim_->auto_move(player);
    }
    CustomAssetController::on_update(in);
}

void spider_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
