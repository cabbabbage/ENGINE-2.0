#include "bomb_controller.hpp"
#include "animation/animation_update.hpp"
#include "animation/attack_validation.hpp"
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/frame_stats_recorder.hpp"

bomb_controller::bomb_controller(Asset* self)
    : CustomAssetController(self),
      steering_(custom_controller_api::EnemyCombatSteeringConfig{
          120,
          72,
          8,
          14,
          150,
          760
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

    if (custom_controller_api::current_animation_has_tag(*self, "die")) {
        state_ = State::Detonating;
    }

    if (state_ == State::Detonating) {
        dispatch_explosion_once(player);
        return;
    }

    if (!player) {
        return;
    }

    constexpr int kApproachRangePx = 120;
    constexpr int kVisitThresholdPx = 80;
    constexpr int kDetonationWindowHorizonFrames = 20;

    std::string detonation_animation_id = "die";
    if (const auto attack_die = self->anim_->resolve_animation_by_tags({"attack", "die"}, {}); attack_die.has_value()) {
        detonation_animation_id = *attack_die;
    } else if (const auto die_only = self->anim_->resolve_animation_by_tags({"die"}, {}); die_only.has_value()) {
        detonation_animation_id = *die_only;
    }

    steering_.tick_progress(*self);
    const auto detonation_window = animation_update::AttackValidation::evaluate_attack_window(
        *self,
        *player,
        detonation_animation_id,
        kDetonationWindowHorizonFrames);
    if (detonation_window.score != animation_update::AttackValidation::AttackWindowScore::Miss) {
        begin_detonation(player, detonation_animation_id);
        dispatch_explosion_once(player);
        return;
    }

    AnimationUpdate::AutoMoveCombatOverrides combat_overrides;
    combat_overrides.attacking_enabled = false;
    steering_.approach(*self, *player, kApproachRangePx, kVisitThresholdPx, false, combat_overrides);
}

void bomb_controller::on_death() {
    begin_detonation(custom_controller_api::resolve_valid_player_target(game_context()), "die");
}

void bomb_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}

void bomb_controller::begin_detonation(Asset* player, const std::string& animation_id) {
    Asset* self = self_ptr();
    if (!self || !self->anim_ || state_ == State::Detonating) {
        return;
    }

    state_ = State::Detonating;
    explosion_dispatched_ = false;
    steering_.reset();
    self->anim_->stop_movement();
    if (!animation_id.empty()) {
        self->anim_->set_animation(animation_id);
    } else if (!self->anim_->set_animation_by_tags({"attack", "die"}, {})) {
        if (!self->anim_->set_animation_by_tags({"die"}, {})) {
            self->anim_->set_animation("die");
        }
    }
    self->needs_target = false;
    self->target_reached = true;

    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.set("combat.bomb_detonated", true);
    if (player) {
        frame_stats.set("combat.bomb_target_health_before", player->runtime_health);
    }
}

void bomb_controller::dispatch_explosion_once(Asset* player) {
    Asset* self = self_ptr();
    if (explosion_dispatched_ || !self || !player || !self->isAttackBoxEnabled()) {
        return;
    }
    if (self->current_attack_box_volumes().empty()) {
        return;
    }

    const auto attack = animation_update::AttackValidation::compute_attack_if_hit(*self, *player);
    if (!attack.has_value()) {
        return;
    }

    player->send_attack(*attack);
    explosion_dispatched_ = true;
    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.set("combat.bomb_explosion_sent", true);
    frame_stats.set("combat.bomb_explosion_damage", attack->payload.damage_amount);
}
