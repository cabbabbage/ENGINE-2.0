#include "bomb_controller.hpp"

#include "assets/asset/Asset.hpp"

bomb_controller::bomb_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    behavior_config_.kamikaze = true;
    behavior_config_.ranges.aggro_radius_px = 120;
    behavior_config_.ranges.desired_standoff_px = 0;
    behavior_config_.ranges.attack_radius_px = 80;
    behavior_config_.retreat_distance_px = 0;
    behavior_config_.recover_ms = 0;
    behavior_config_.attack_window_ms = 200;
    behavior_config_.return_home_threshold_px = 0;
    behavior_config_.force_attacking_enabled = true;
    chase_move_.visit_threshold_px = 10;
    chase_move_.override_non_locked = false;
    chase_move_.allow_vertical_movement = false;
    retreat_move_.visit_threshold_px = 10;
    retreat_move_.override_non_locked = false;
    retreat_move_.allow_vertical_movement = false;
    Asset* owner = controller_self();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void bomb_controller::on_update(const Input& in) {
    custom_controller_api::CustomControllerBase::on_update(in);
    const auto& ctx = controller_game_context();
    Asset* self = controller_self();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = resolve_target_player();
    run_enemy_behavior(player, behavior_config_, chase_move_, retreat_move_);
}

void bomb_controller::on_death() {
    Asset* self = controller_self();
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
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self);
}

