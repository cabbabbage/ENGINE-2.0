#include "boneski_controller.hpp"

#include "assets/asset/Asset.hpp"

boneski_controller::boneski_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    behavior_config_.kamikaze = false;
    behavior_config_.ranges.aggro_radius_px = 240;
    behavior_config_.ranges.desired_standoff_px = 10;
    behavior_config_.ranges.attack_radius_px = 74;
    behavior_config_.retreat_distance_px = 220;
    behavior_config_.recover_ms = 430;
    behavior_config_.attack_window_ms = 220;
    behavior_config_.return_home_threshold_px = 110;
    behavior_config_.force_attacking_enabled = true;

    chase_move_.visit_threshold_px = 12;
    chase_move_.override_non_locked = true;
    chase_move_.allow_vertical_movement = false;
    retreat_move_.visit_threshold_px = 12;
    retreat_move_.override_non_locked = true;
    retreat_move_.allow_vertical_movement = false;

    Asset* owner = controller_self();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
        owner->set_default_controller_animation_enforced(false);
    }
}

void boneski_controller::on_update(const Input& in) {
    custom_controller_api::CustomControllerBase::on_update(in);
    const auto& ctx = controller_game_context();
    Asset* self = ctx.self;
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = resolve_target_player();
    if (player && !ctx.self_and_player_share_room()) {
        player = nullptr;
    }

    run_enemy_behavior(player, behavior_config_, chase_move_, retreat_move_);
    if (player && behavior_state().mode == custom_controller_api::EnemyAgentPhase::AttackWindow) {
        (void)face_target(*player);
        (void)try_attack_target(*player,
                                "boneski_primary",
                                0.7f,
                                behavior_config_.ranges.attack_radius_px + 24,
                                "attack_right");
    }
}
