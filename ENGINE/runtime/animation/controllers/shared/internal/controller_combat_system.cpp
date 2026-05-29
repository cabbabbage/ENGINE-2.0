#include "animation/controllers/shared/internal/controller_combat_system.hpp"

#include <algorithm>

#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "assets/asset/Asset.hpp"

namespace animation_update::custom_controllers::internal {

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
        if (self.anim_->set_animation_by_tags(required_tags, excluded_tags)) {
            return true;
        }
    }
    if (!animation_id.empty()) {
        self.anim_->set_animation(animation_id);
        return true;
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
        return false;
    }
    if (!cooldown_ready(cooldowns, cooldown_key)) {
        return false;
    }

    (void)start_attack_animation(self, animation_id, required_tags, excluded_tags);
    const bool hit = apply_attack_hit(self, target);
    if (hit) {
        start_cooldown(cooldowns, cooldown_key, cooldown_seconds);
    }
    return hit;
}

bool ControllerCombatSystem::apply_attack_hit(Asset& attacker, Asset& target) {
    AttackDetectionHelper::send_attack_if_hit(&attacker, &target);
    return true;
}

bool ControllerCombatSystem::apply_attack_hits_to_active_targets(Asset& attacker, Assets* assets) {
    if (!assets) {
        return false;
    }
    AttackDetectionHelper::send_attacks_to_active_targets(&attacker, assets);
    return true;
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
