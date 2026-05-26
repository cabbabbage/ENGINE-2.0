#include "spider_controller.hpp"

#include "assets/asset/Asset.hpp"

spider_controller::spider_controller(Asset* self)
    : CustomAssetController(self),
      steering_(custom_controller_api::EnemyCombatSteeringConfig{
          150,
          80,
          8,
          14,
          210,
          820
      }),
      behavior_(custom_controller_api::EnemyAutoCombatConfig{
          custom_controller_api::EnemyAutoCombatMode::SkirmisherShortEvade,
          230,
          90,
          310,
          130,
          520,
          false
      }) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
        owner->set_default_controller_animation_enforced(false);
    }
}

void spider_controller::on_update(const Input& in) {
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

void spider_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}

