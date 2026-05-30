#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "animation/controllers/shared/controller_types.hpp"

class Asset;
class Assets;

namespace animation_update {
struct Attack;
}

namespace animation_update::custom_controllers::internal {

class ControllerCombatSystem {
public:
    using Clock = std::chrono::steady_clock;

    struct CooldownState {
        std::unordered_map<std::string, Clock::time_point> expiry_by_key;
    };

    static bool is_target_in_range(const Asset& self, const Asset& target, int range_px);
    static bool cooldown_ready(const CooldownState& cooldowns, const std::string& key);
    static void start_cooldown(CooldownState& cooldowns, const std::string& key, float seconds);

    static bool start_attack_animation(Asset& self,
                                       const std::string& animation_id,
                                       const std::vector<std::string>& required_tags = {},
                                       const std::vector<std::string>& excluded_tags = {});
    static bool try_attack_target(Asset& self,
                                  Asset& target,
                                  CooldownState& cooldowns,
                                  const std::string& cooldown_key,
                                  float cooldown_seconds,
                                  int range_px,
                                  const std::string& animation_id = {},
                                  const std::vector<std::string>& required_tags = {},
                                  const std::vector<std::string>& excluded_tags = {});
    static bool should_start_cooldown_after_attack(bool hit_dispatched) { return hit_dispatched; }
    static bool has_any_dispatched_hits(int hit_count) { return hit_count > 0; }

    static bool apply_attack_hit(Asset& attacker, Asset& target);
    static int apply_attack_hits_to_active_targets_count(Asset& attacker, Assets* assets);
    static bool apply_attack_hits_to_active_targets(Asset& attacker, Assets* assets);

    static bool is_hit_window_open(const Asset& self, int window_start_frame, int window_end_frame);

    static AttackProcessingSummary process_pending_attacks(Asset& self,
                                                           const AttackProcessingConfig& config);
    static AttackProcessingSummary process_attacks(Asset& self,
                                                   const std::vector<animation_update::Attack>& attacks,
                                                   const AttackProcessingConfig& config);
};

} // namespace animation_update::custom_controllers::internal
