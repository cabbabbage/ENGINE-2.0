#include "spider_controller.hpp"

#include "assets/asset/Asset.hpp"
#include <optional>

spider_controller::spider_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    behavior_config_.kamikaze = false;
    behavior_config_.chase_range_px = 230;
    behavior_config_.attack_range_px = 90;
    behavior_config_.retreat_distance_px = 310;
    behavior_config_.recover_ms = 520;
    behavior_config_.return_home_threshold_px = 130;
    behavior_config_.force_attacking_enabled = false;
    chase_move_.visit_threshold_px = 12;
    retreat_move_.visit_threshold_px = 12;
    chase_move_.resolution_layer = std::nullopt;
    retreat_move_.resolution_layer = std::nullopt;
    chase_move_.override_non_locked = false;
    retreat_move_.override_non_locked = false;
    Asset* owner = controller_self();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
        owner->set_default_controller_animation_enforced(false);
    }
}

void spider_controller::on_update(const Input& in) {
    (void)in;
    const auto& ctx = controller_game_context();
    Asset* self = controller_self();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }
    Asset* player = resolve_target_player();
    run_enemy_behavior(player, behavior_config_, chase_move_, retreat_move_);
}

void spider_controller::on_process_pending_attacks(Asset& self) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self);
}

