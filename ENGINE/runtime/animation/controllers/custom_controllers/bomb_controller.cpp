#include "bomb_controller.hpp"

#include "animation/animation_update.hpp"
#include "animation/attack.hpp"
#include "animation/controllers/shared/attack_payload.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <unordered_set>

namespace {

constexpr int kDetonationTriggerRadiusPx = 42;
constexpr int kExplosionRadiusPx = 130;
constexpr int kExplosionDamage = 70;
constexpr float kExplosionHitbackDistancePx = 140.0f;
constexpr int kGroundContactBufferPx = 2;

long long distance_sq_3d(const Asset& a, const Asset& b) {
    const long long dx = static_cast<long long>(b.world_x()) - static_cast<long long>(a.world_x());
    const long long dy = static_cast<long long>(b.world_y()) - static_cast<long long>(a.world_y());
    const long long dz = static_cast<long long>(b.world_z()) - static_cast<long long>(a.world_z());
    return (dx * dx) + (dy * dy) + (dz * dz);
}

} // namespace

bomb_controller::bomb_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    behavior_config_.kamikaze = true;
    behavior_config_.ranges.aggro_radius_px = 190;
    behavior_config_.ranges.desired_standoff_px = 0;
    behavior_config_.ranges.attack_radius_px = kDetonationTriggerRadiusPx;
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
        owner->set_default_controller_animation_enforced(false);
    }
}

void bomb_controller::on_update(const Input& in) {
    custom_controller_api::CustomControllerBase::on_update(in);
    const auto& ctx = controller_game_context();
    Asset* self = ctx.self;
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }
    if (has_detonated_) {
        return;
    }

    Asset* player = resolve_target_player();
    if (player && !ctx.self_and_player_share_room()) {
        player = nullptr;
    }

    if (player && can_detonate(*self, *player)) {
        detonate(*self, *player);
        return;
    }

    if (!player && self->anim_->debug_enabled()) {
        vibble::log::info("[AICombat] Bomb could not acquire player target");
    }
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

bool bomb_controller::can_detonate(const Asset& self, const Asset& target) const {
    Assets* owner_assets = self.get_assets();
    if (!owner_assets) {
        return false;
    }

    const world::GridPoint floor_point =
        owner_assets->resolve_floor_world_point(SDL_Point{self.world_x(), self.world_z()}, self.grid_resolution);
    if (self.world_y() > floor_point.world_y() + kGroundContactBufferPx) {
        return false;
    }

    const long long trigger_sq = static_cast<long long>(kDetonationTriggerRadiusPx) *
                                 static_cast<long long>(kDetonationTriggerRadiusPx);
    return distance_sq_3d(self, target) <= trigger_sq;
}

void bomb_controller::detonate(Asset& self, Asset& target) {
    (void)target;
    if (has_detonated_) {
        return;
    }
    has_detonated_ = true;
    dispatch_explosion_attacks(self);

    animation_update::Attack self_kill{};
    self_kill.attacker_asset_id = animation_update::detail::stable_asset_id(self);
    self_kill.attacker_asset_name = self.info ? self.info->name : std::string{"bomb"};
    self_kill.target_asset_id = animation_update::detail::stable_asset_id(self);
    self_kill.target_asset_name = self_kill.attacker_asset_name;
    self_kill.attack_type = "bomb_self_detonation";
    self_kill.payload = animation_update::make_default_attack_payload();
    self_kill.payload.payload_id = "bomb_self_detonation";
    self_kill.payload.damage_amount = std::max(1, self.runtime_health);
    self_kill.payload.hitback_enabled = false;
    self_kill.damage_amount = self_kill.payload.damage_amount;
    self_kill.attack_payload_id = self_kill.payload.payload_id;
    self_kill.hit_x = static_cast<float>(self.world_x());
    self_kill.hit_y = static_cast<float>(self.world_y());
    self_kill.hit_z = static_cast<float>(self.world_z());
    self.send_attack(self_kill);
}

void bomb_controller::dispatch_explosion_attacks(Asset& self) {
    Assets* owner_assets = self.get_assets();
    if (!owner_assets) {
        return;
    }

    const long long explosion_sq = static_cast<long long>(kExplosionRadiusPx) *
                                   static_cast<long long>(kExplosionRadiusPx);
    const std::string attacker_id = animation_update::detail::stable_asset_id(self);
    const std::string attacker_name = self.info ? self.info->name : std::string{"bomb"};
    std::unordered_set<std::string> dispatched_target_ids;

    auto dispatch_to_target = [&](Asset* target) {
        if (!target || target == &self || !target->active || target->dead || !target->isHitboxEnabled()) {
            return;
        }
        const std::string target_id = animation_update::detail::stable_asset_id(*target);
        if (target_id.empty() || !dispatched_target_ids.emplace(target_id).second) {
            return;
        }
        if (!self.owning_room_name().empty() &&
            !target->owning_room_name().empty() &&
            self.owning_room_name() != target->owning_room_name()) {
            return;
        }
        if (distance_sq_3d(self, *target) > explosion_sq) {
            return;
        }

        animation_update::Attack attack{};
        attack.attacker_asset_id = attacker_id;
        attack.attacker_asset_name = attacker_name;
        attack.target_asset_id = target_id;
        attack.target_asset_name = target->info ? target->info->name : std::string{};
        attack.attack_type = "bomb_explosion";
        attack.payload = animation_update::make_default_attack_payload();
        attack.payload.payload_id = "bomb_explosion";
        attack.payload.damage_amount = kExplosionDamage;
        attack.payload.hitback_enabled = true;
        attack.payload.hitback_distance = kExplosionHitbackDistancePx;
        attack.damage_amount = attack.payload.damage_amount;
        attack.attack_payload_id = attack.payload.payload_id;
        attack.hit_x = static_cast<float>(self.world_x());
        attack.hit_y = static_cast<float>(self.world_y());
        attack.hit_z = static_cast<float>(self.world_z());
        target->send_attack(attack);
    };

    for (Asset* candidate : owner_assets->getActive()) {
        dispatch_to_target(candidate);
    }
    dispatch_to_target(owner_assets->player);
}

