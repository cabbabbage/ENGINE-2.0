#include "animation/controllers/shared/internal/enemy_combat_coordinator.hpp"

#include <algorithm>
#include <cmath>

#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/frame_stats_recorder.hpp"

namespace animation_update::custom_controllers::internal {

EnemyAttackProfile EnemyCombatCoordinator::make_legacy_profile(
    const std::string& id,
    float cooldown_seconds,
    int range_px,
    const std::string& animation_id,
    const std::vector<std::string>& required_tags,
    const std::vector<std::string>& excluded_tags) {
    EnemyAttackProfile profile{};
    profile.id = id;
    profile.animation_id = animation_id;
    profile.required_tags = required_tags;
    profile.excluded_tags = excluded_tags;
    profile.max_range_px = std::max(0, range_px);
    profile.active_start_frame = 0;
    profile.active_end_frame = 9999;
    profile.cooldown_ms_on_start =
        static_cast<int>(std::lround(std::max(0.0f, cooldown_seconds) * 1000.0f));
    profile.cooldown_ms_on_whiff = profile.cooldown_ms_on_start;
    profile.cooldown_ms_on_hit = profile.cooldown_ms_on_start;
    profile.allow_contact_fallback = false;
    if (!animation_id.empty()) {
        profile.id = id.empty() ? animation_id : id;
    }
    return profile;
}

AttackRequestResult EnemyCombatCoordinator::commit_startup(Asset& self,
                                                           Asset& target,
                                                           const EnemyAttackProfile& profile) {
    AttackRequestResult result{};
    result.profile = profile;
    result.accepted = true;
    result.reason = "attack_commit_started";
    result.state.phase = AttackCommitPhase::Startup;
    result.state.attacker_id = animation_update::detail::stable_asset_id(self);
    result.state.target_id = animation_update::detail::stable_asset_id(target);
    result.state.profile_id = profile.id;
    result.state.animation_id = self.current_animation.empty() ? profile.animation_id : self.current_animation;
    result.state.path_index = 0;
    if (const auto* frame = self.current_animation_frame()) {
        result.state.frame_started = frame->frame_index;
    }

    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.add("enemy_ai.attack_commit_start_count", 1.0);
    frame_stats.set("enemy_ai.attack_profile", result.profile.id);
    frame_stats.set("enemy_ai.attack_phase", phase_name(result.state.phase));
    frame_stats.set("enemy_ai.attack_contact_fallback_enabled", result.profile.allow_contact_fallback);
    return result;
}

const char* EnemyCombatCoordinator::phase_name(AttackCommitPhase phase) {
    switch (phase) {
    case AttackCommitPhase::None: return "None";
    case AttackCommitPhase::Startup: return "Startup";
    case AttackCommitPhase::Active: return "Active";
    case AttackCommitPhase::Recovery: return "Recovery";
    case AttackCommitPhase::Complete: return "Complete";
    case AttackCommitPhase::Interrupted: return "Interrupted";
    }
    return "Unknown";
}

long long EnemyCombatCoordinator::horizontal_distance_sq(const Asset& self, const Asset& target) {
    const long long dx = static_cast<long long>(target.world_x()) - static_cast<long long>(self.world_x());
    const long long dz = static_cast<long long>(target.world_z()) - static_cast<long long>(self.world_z());
    return (dx * dx) + (dz * dz);
}

int EnemyCombatCoordinator::vertical_delta_px(const Asset& self, const Asset& target) {
    return std::abs(target.world_y() - self.world_y());
}

} // namespace animation_update::custom_controllers::internal
