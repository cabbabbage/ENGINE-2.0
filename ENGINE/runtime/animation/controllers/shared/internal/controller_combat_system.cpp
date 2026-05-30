#include "animation/controllers/shared/internal/controller_combat_system.hpp"

#include <algorithm>
#include <utility>

#include "animation/controllers/shared/internal/controller_attack_detection_helper.hpp"
#include "animation/controllers/shared/internal/controller_attack_processing_helper.hpp"
#include "animation/animation_update.hpp"
#include "animation/controllers/shared/attack_payload.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_types.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "utils/log.hpp"

namespace animation_update::custom_controllers::internal {

namespace {

animation_update::AttackPayload payload_from_current_attack_box(const Asset& self) {
    for (const auto& volume : self.current_attack_box_volumes()) {
        if (!volume.enabled || !volume.valid) {
            continue;
        }
        animation_update::AttackPayload payload = volume.payload;
        if (payload.payload_id.empty()) {
            payload.payload_id = volume.payload_id.empty() ? volume.id : volume.payload_id;
        }
        if (payload.damage_amount <= 0) {
            payload.damage_amount = std::max(0, volume.damage_amount);
        }
        if (payload.damage_amount > 0) {
            return animation_update::sanitize_attack_payload(std::move(payload));
        }
    }

    auto payload = animation_update::make_default_attack_payload();
    payload.payload_id = "enemy_contact_attack";
    payload.damage_amount = 20;
    payload.hitback_enabled = true;
    payload.hitback_distance = 60.0f;
    return animation_update::sanitize_attack_payload(std::move(payload));
}

bool dispatch_enemy_contact_hit(Asset& attacker, Asset& target) {
    animation_update::Attack attack{};
    attack.attacker_asset_id = animation_update::detail::stable_asset_id(attacker);
    attack.attacker_asset_name = attacker.info ? attacker.info->name : std::string{};
    attack.target_asset_id = animation_update::detail::stable_asset_id(target);
    attack.target_asset_name = target.info ? target.info->name : std::string{};
    attack.attack_type = "enemy_contact_attack";
    attack.payload = payload_from_current_attack_box(attacker);
    attack.attack_payload_id = attack.payload.payload_id;
    attack.damage_amount = attack.payload.damage_amount;
    attack.hit_x = static_cast<float>(attacker.world_x() + target.world_x()) * 0.5f;
    attack.hit_y = static_cast<float>(attacker.world_y() + target.world_y()) * 0.5f;
    attack.hit_z = static_cast<float>(attacker.world_z() + target.world_z()) * 0.5f;
    if (const auto* frame = attacker.current_animation_frame()) {
        attack.source_frame_index = frame->frame_index;
    }
    target.send_attack(attack);
    return true;
}

} // namespace

bool ControllerCombatSystem::is_target_in_range(const Asset& self, const Asset& target, int range_px) {
    const long long dx = static_cast<long long>(target.world_x()) - static_cast<long long>(self.world_x());
    const long long dy = static_cast<long long>(target.world_y()) - static_cast<long long>(self.world_y());
    const long long dz = static_cast<long long>(target.world_z()) - static_cast<long long>(self.world_z());
    const long long dist_sq = (dx * dx) + (dy * dy) + (dz * dz);
    const long long range = std::max(0, range_px);
    return dist_sq <= (range * range);
}

bool ControllerCombatSystem::cooldown_ready(const CooldownState& cooldowns, const std::string& key) {
    if (key.empty()) {
        return true;
    }
    const auto it = cooldowns.expiry_by_key.find(key);
    if (it == cooldowns.expiry_by_key.end()) {
        return true;
    }
    return Clock::now() >= it->second;
}

void ControllerCombatSystem::start_cooldown(CooldownState& cooldowns,
                                            const std::string& key,
                                            float seconds) {
    if (key.empty()) {
        return;
    }
    const float clamped = std::max(0.0f, seconds);
    cooldowns.expiry_by_key[key] = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(clamped));
}

bool ControllerCombatSystem::start_attack_animation(Asset& self,
                                                    const std::string& animation_id,
                                                    const std::vector<std::string>& required_tags,
                                                    const std::vector<std::string>& excluded_tags) {
    if (!self.anim_) {
        return false;
    }
    if (!required_tags.empty() || !excluded_tags.empty()) {
        if (self.anim_->set_animation_by_tags_deterministic(required_tags, excluded_tags, true)) {
            return true;
        }
    }
    if (!animation_id.empty()) {
        return self.anim_->set_animation(animation_id, true);
    }
    if (self.anim_->debug_enabled()) {
        const std::string self_name =
            (self.info && !self.info->name.empty()) ? self.info->name : std::string{"<unknown>"};
        vibble::log::info("[AICombat] Attack requested but no valid attack animation resolved for '" + self_name + "'");
    }
    return false;
}

bool ControllerCombatSystem::try_attack_target(Asset& self,
                                               Asset& target,
                                               CooldownState& cooldowns,
                                               const std::string& cooldown_key,
                                               float cooldown_seconds,
                                               int range_px,
                                               const std::string& animation_id,
                                               const std::vector<std::string>& required_tags,
                                               const std::vector<std::string>& excluded_tags) {
    if (!is_target_in_range(self, target, range_px)) {
        if (self.anim_ && self.anim_->debug_enabled()) {
            vibble::log::info("[AICombat] Attack rejected: target out of range");
        }
        return false;
    }
    if (!cooldown_ready(cooldowns, cooldown_key)) {
        if (self.anim_ && self.anim_->debug_enabled()) {
            vibble::log::info("[AICombat] Attack rejected: cooldown active for key '" + cooldown_key + "'");
        }
        return false;
    }

    const bool attack_started =
        start_attack_animation(self, animation_id, required_tags, excluded_tags);
    if (!attack_started) {
        return false;
    }

    const bool enemy_attacker =
        self.info && asset_types::canonicalize(self.info->type) == asset_types::enemy;
    if (enemy_attacker) {
        self.update_anchor_basis_if_needed();
        self.refresh_anchor_point_cache_from_frame();
        self.refresh_runtime_box_cache_from_frame();

        bool hit = apply_attack_hit(self, target);
        if (!hit) {
            // Custom enemy controllers are allowed to own combat. If authored attack boxes
            // miss because of frame/path metadata drift, still land the close-range commit
            // on the intended player target instead of silently doing nothing.
            hit = dispatch_enemy_contact_hit(self, target);
            auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
            frame_stats.add("enemy_ai.hit_dispatch_success_count", 1.0);
        }
        if (should_start_cooldown_after_attack(hit)) {
            start_cooldown(cooldowns, cooldown_key, cooldown_seconds);
        }
        if (self.anim_ && self.anim_->debug_enabled()) {
            vibble::log::info(std::string{"[AICombat] Enemy attack committed to player target; dispatched="} +
                              (hit ? "true" : "false"));
        }
        return hit;
    }

    const bool hit = apply_attack_hit(self, target);
    if (should_start_cooldown_after_attack(hit)) {
        start_cooldown(cooldowns, cooldown_key, cooldown_seconds);
    }
    return hit;
}

bool ControllerCombatSystem::apply_attack_hit(Asset& attacker, Asset& target) {
    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.add("enemy_ai.hit_dispatch_attempt_count", 1.0);
    const bool hit = AttackDetectionHelper::send_attack_if_hit(&attacker, &target);
    if (hit) {
        frame_stats.add("enemy_ai.hit_dispatch_success_count", 1.0);
    }
    return hit;
}

int ControllerCombatSystem::apply_attack_hits_to_active_targets_count(Asset& attacker, Assets* assets) {
    if (!assets) {
        return 0;
    }
    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.add("enemy_ai.hit_dispatch_attempt_count", 1.0);
    const int hit_count = AttackDetectionHelper::send_attacks_to_active_targets(&attacker, assets);
    frame_stats.add("enemy_ai.active_target_scan_hits", static_cast<double>(hit_count));
    if (hit_count > 0) {
        frame_stats.add("enemy_ai.hit_dispatch_success_count", static_cast<double>(hit_count));
    }
    return hit_count;
}

bool ControllerCombatSystem::apply_attack_hits_to_active_targets(Asset& attacker, Assets* assets) {
    return has_any_dispatched_hits(apply_attack_hits_to_active_targets_count(attacker, assets));
}

bool ControllerCombatSystem::is_hit_window_open(const Asset& self,
                                                int window_start_frame,
                                                int window_end_frame) {
    const auto* frame = self.current_animation_frame();
    if (!frame) {
        return false;
    }
    const int start = std::min(window_start_frame, window_end_frame);
    const int end = std::max(window_start_frame, window_end_frame);
    return frame->frame_index >= start && frame->frame_index <= end;
}

AttackProcessingSummary ControllerCombatSystem::process_pending_attacks(
    Asset& self,
    const AttackProcessingConfig& config) {
    return AttackProcessingHelper::process_attacks(self, self.process_pending_attacks(), config);
}

AttackProcessingSummary ControllerCombatSystem::process_attacks(
    Asset& self,
    const std::vector<animation_update::Attack>& attacks,
    const AttackProcessingConfig& config) {
    return AttackProcessingHelper::process_attacks(self, attacks, config);
}

} // namespace animation_update::custom_controllers::internal
