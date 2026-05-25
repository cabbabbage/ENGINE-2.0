#include "spider_controller.hpp"

#include "animation/animation_update.hpp"
#include "animation/attack_validation.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>

namespace {

constexpr int kAttackEnterRangePx = 330;
constexpr int kAttackApproachRangePx = 230;
constexpr int kAttackVisitThresholdPx = 90;
constexpr int kEvadeDistancePx = 310;
constexpr int kEvadeVisitThresholdPx = 130;
constexpr int kEvadeMs = 520;
constexpr int kAttackCooldownMs = 680;
constexpr int kAttackWindowHorizonFrames = 16;

std::string stable_asset_name_for_attack(const Asset& asset) {
    return asset.info ? asset.info->name : std::string{};
}

} // namespace

spider_controller::spider_controller(Asset* self)
    : CustomAssetController(self),
      steering_(custom_controller_api::EnemyCombatSteeringConfig{
          150,
          80,
          8,
          14,
          210,
          820
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

    if (custom_controller_api::current_animation_has_tag(*self, "attack")) {
        state_ = State::Attack;
    } else if (state_ == State::Attack) {
        enter_evade();
    }

    switch (state_) {
    case State::Approach:
        tick_approach(*self, *player);
        break;
    case State::Attack:
        tick_attack(*self, *player);
        break;
    case State::Evade:
        tick_evade(*self, *player);
        break;
    }
}

void spider_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}

void spider_controller::tick_approach(Asset& self, Asset& player) {
    const auto now = std::chrono::steady_clock::now();
    const long long attack_range_sq =
        static_cast<long long>(kAttackEnterRangePx) * kAttackEnterRangePx;
    const auto attack_animation_id = resolve_attack_animation_id(self, player);
    bool window_hittable = false;
    if (attack_animation_id.has_value()) {
        window_hittable = attack_window_is_hittable(self, player, *attack_animation_id);
    }
    const bool fresh_hittable_window = window_hittable && (!prior_window_hittable_ || !await_fresh_window_after_evade_);
    prior_window_hittable_ = window_hittable;

    if (now >= next_attack_time_ &&
        custom_controller_api::distance_sq_xz(self, player) <= attack_range_sq &&
        attack_animation_id.has_value() &&
        fresh_hittable_window &&
        trigger_attack(self, player, *attack_animation_id)) {
        (void)dispatch_attack_frame_once(self, player);
        return;
    }

    AnimationUpdate::AutoMoveCombatOverrides combat_overrides;
    combat_overrides.attacking_enabled = true;
    steering_.approach(self,
                       player,
                       kAttackApproachRangePx,
                       kAttackVisitThresholdPx,
                       false,
                       combat_overrides);
}

void spider_controller::tick_attack(Asset& self, Asset& player) {
    if (!custom_controller_api::current_animation_has_tag(self, "attack")) {
        enter_evade();
        return;
    }

    if (dispatch_attack_frame_once(self, player)) {
        auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
        frame_stats.set("combat.spider_attack_sent", true);
        frame_stats.set("combat.vibble_health_before_spider_attack", player.runtime_health);
    }
}

void spider_controller::tick_evade(Asset& self, Asset& player) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= evade_until_) {
        state_ = State::Approach;
        steering_.reset();
        await_fresh_window_after_evade_ = true;
        return;
    }

    AnimationUpdate::AutoMoveCombatOverrides combat_overrides;
    combat_overrides.attacking_enabled = false;
    steering_.evade(self,
                    player,
                    kEvadeDistancePx,
                    kEvadeVisitThresholdPx,
                    self.needs_target || self.target_reached || steering_.is_stuck(),
                    combat_overrides);
}

std::optional<std::string> spider_controller::resolve_attack_animation_id(Asset& self, const Asset& player) const {
    if (!self.anim_) {
        return std::nullopt;
    }
    const char* facing_tag = player.world_x() < self.world_x() ? "left" : "right";
    if (const auto facing_attack = self.anim_->resolve_animation_by_tags({"attack", facing_tag}, {});
        facing_attack.has_value()) {
        return facing_attack;
    }
    return self.anim_->resolve_animation_by_tags({"attack"}, {});
}

bool spider_controller::attack_window_is_hittable(const Asset& self,
                                                  const Asset& player,
                                                  const std::string& attack_animation_id) const {
    const auto evaluation = animation_update::AttackValidation::evaluate_attack_window(
        self,
        player,
        attack_animation_id,
        kAttackWindowHorizonFrames);
    return evaluation.score != animation_update::AttackValidation::AttackWindowScore::Miss;
}

bool spider_controller::trigger_attack(Asset& self,
                                       const Asset& player,
                                       const std::string& attack_animation_id) {
    if (!self.anim_) {
        return false;
    }
    if (attack_animation_id.empty()) {
        return false;
    }

    self.anim_->stop_movement();
    self.anim_->set_animation(attack_animation_id);

    state_ = State::Attack;
    steering_.reset();
    last_dispatched_attack_frame_ = -1;
    last_dispatched_payload_id_.clear();
    attack_dispatched_this_cycle_ = false;
    await_fresh_window_after_evade_ = false;
    prior_window_hittable_ = true;
    next_attack_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAttackCooldownMs);

    std::ostringstream oss;
    oss << "[Combat] Spider attack started target='" << stable_asset_name_for_attack(player)
        << "' distance_sq=" << custom_controller_api::distance_sq_xz(self, player);
    vibble::log::info(oss.str());
    return true;
}

bool spider_controller::dispatch_attack_frame_once(Asset& self, Asset& player) {
    if (attack_dispatched_this_cycle_) {
        return false;
    }

    const auto attack_opt = animation_update::AttackValidation::compute_attack_if_hit(self, player);
    if (!attack_opt.has_value()) {
        return false;
    }

    const auto& attack = *attack_opt;
    const std::string payload_id =
        attack.payload.payload_id.empty() ? attack.attack_payload_id : attack.payload.payload_id;
    if (attack.source_frame_index == last_dispatched_attack_frame_ &&
        payload_id == last_dispatched_payload_id_) {
        return false;
    }

    player.send_attack(attack);
    last_dispatched_attack_frame_ = attack.source_frame_index;
    last_dispatched_payload_id_ = payload_id;
    attack_dispatched_this_cycle_ = true;
    return true;
}

void spider_controller::enter_evade() {
    state_ = State::Evade;
    steering_.reset();
    last_dispatched_attack_frame_ = -1;
    last_dispatched_payload_id_.clear();
    attack_dispatched_this_cycle_ = false;
    await_fresh_window_after_evade_ = true;
    prior_window_hittable_ = true;
    evade_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kEvadeMs);
}
