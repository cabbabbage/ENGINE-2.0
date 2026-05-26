#include "bomb_controller.hpp"

#include "assets/asset/Asset.hpp"

bomb_controller::bomb_controller(Asset* self)
    : CustomAssetController(self),
      steering_(custom_controller_api::EnemyCombatSteeringConfig{
          120,
          72,
          8,
          14,
          150,
          760
      }),
      behavior_(custom_controller_api::EnemyAutoCombatConfig{
          custom_controller_api::EnemyAutoCombatMode::KamikazeDetonate,
          120,
          80,
          0,
          0,
          0,
          true
      }) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void bomb_controller::on_update(const Input& in) {
    (void)in;
    const auto& ctx = game_context();
    Asset* self = self_ptr();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = custom_controller_api::resolve_valid_player_target(ctx);
    if (!player) {
        return;
    }

    steering_.tick_progress(*self);
    behavior_.tick(*self, *player, steering_);
}

void bomb_controller::on_death() {
    Asset* self = self_ptr();
    if (!self || !self->anim_) {
        return;
    }
    // Preserve death fallback even if no player target is currently valid.
    if (!self->anim_->set_animation_by_tags({"attack", "die"}, {})) {
        if (!self->anim_->set_animation_by_tags({"die"}, {})) {
            self->anim_->set_animation("die");
        }
    }
}

void bomb_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}

