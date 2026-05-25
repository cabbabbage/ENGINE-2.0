#include "spider_controller.hpp"
#include "animation/animation_update.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "assets/asset/Asset.hpp"

spider_controller::spider_controller(Asset* self)
    : CustomAssetController(self) {
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

    if (self->needs_target) {
        AnimationUpdate::AutoMoveCombatOverrides combat_overrides{};
        combat_overrides.attacking_enabled = true;
        self->anim_->auto_move(player, 0, true, combat_overrides);
    }

    // Auto-move combat animations do not always trigger the generic controller
    // attack dispatch path, so explicitly dispatch runtime attack-box collisions
    // from spider to valid active targets (including Vibble).
    animation_update::custom_controllers::AttackDetectionHelper::send_attacks_to_active_targets(
        self,
        ctx.assets);
}

void spider_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
