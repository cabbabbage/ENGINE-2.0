#include "bomb_controller.hpp"
#include "animation/controllers/shared/custom_controller_update_utils.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/range_util.hpp"

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

    const auto& ctx = game_context();
    Asset* self = self_ptr();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }
    Asset* player = animation_update::custom_controllers::resolve_valid_player_target(ctx);
    if (!player) {
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

    animation_update::custom_controllers::dispatch_contact_attack(ctx);
}

void bomb_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
