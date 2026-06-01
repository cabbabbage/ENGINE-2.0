#include "animation/controllers/shared/internal/enemy_combat_coordinator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
    profile.id = id.empty() ? animation_id : id;
    profile.animation_id = animation_id;
    profile.required_tags = required_tags;
    profile.excluded_tags = excluded_tags;
    profile.max_range_px = std::max(0, range_px);
    profile.vertical_tolerance_px = 56;
    profile.startup_frames = 0;
    profile.active_start_frame = 0;
    profile.active_end_frame = 9999;
    profile.recovery_frames = 0;
    profile.cooldown_ms_on_start =
        static_cast<int>(std::lround(std::max(0.0f, cooldown_seconds) * 1000.0f));
    profile.cooldown_ms_on_whiff = profile.cooldown_ms_on_start;
    profile.cooldown_ms_on_hit = profile.cooldown_ms_on_start;
    profile.cooldown_ms_on_interrupt = profile.cooldown_ms_on_start;
    profile.cooldown_ms_on_recovery_complete = 0;
    profile.prediction_horizon_frames = 8;
    profile.prediction_padding_px = 6;
    profile.allow_contact_fallback = false;
    return profile;
}

EnemyAttackProfile EnemyCombatCoordinator::make_contact_hazard_profile(const std::string& id,
                                                                       int cooldown_frames,
                                                                       int contact_range_px) {
    EnemyAttackProfile profile{};
    profile.id = id;
    profile.animation_id = animation_update::detail::kDefaultAnimation;
    profile.max_range_px = std::max(0, contact_range_px);
    profile.vertical_tolerance_px = std::max(8, contact_range_px);
    profile.active_start_frame = 0;
    profile.active_end_frame = 0;
    profile.cooldown_ms_on_hit = std::max(0, cooldown_frames) * 16;
    profile.cooldown_ms_on_whiff = profile.cooldown_ms_on_hit;
    profile.requires_facing = false;
    profile.allow_contact_fallback = true;
    return profile;
}

EnemyAttackProfile EnemyCombatCoordinator::make_explosion_profile(const std::string& id,
                                                                  int arming_frames,
                                                                  int active_frames,
                                                                  int explosion_radius_px,
                                                                  int) {
    EnemyAttackProfile profile{};
    profile.id = id;
    profile.animation_id = "die";
    profile.required_tags = {"attack", "die"};
    profile.max_range_px = std::max(0, explosion_radius_px);
    profile.vertical_tolerance_px = std::max(24, explosion_radius_px / 2);
    profile.startup_frames = std::max(0, arming_frames);
    profile.active_start_frame = profile.startup_frames;
    profile.active_end_frame = profile.startup_frames + std::max(0, active_frames - 1);
    profile.recovery_frames = 1;
    profile.cooldown_ms_on_start = 0;
    profile.cooldown_ms_on_hit = 0;
    profile.cooldown_ms_on_whiff = 0;
    profile.requires_facing = false;
    profile.allow_contact_fallback = false;
    return profile;
}

AttackRequestResult EnemyCombatCoordinator::commit_startup(Asset& self,
                                                           Asset& target,
                                                           const EnemyAttackProfile& profile) {
    AttackRequestResult result{};
    result.profile = profile;
    if (!profile_in_range(profile, self, target)) {
        result.reason = "target_outside_profile_range";
        return result;
    }
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

bool EnemyCombatCoordinator::profile_in_range(const EnemyAttackProfile& profile,
                                              const Asset& self,
                                              const Asset& target) {
    const long long dist_sq = horizontal_distance_sq(self, target);
    const long long min_range = std::max(0, profile.min_range_px);
    const long long max_range = std::max(0, profile.max_range_px);
    if (dist_sq < min_range * min_range) {
        return false;
    }
    if (max_range > 0 && dist_sq > max_range * max_range) {
        return false;
    }
    return std::abs(vertical_delta_px(self, target)) <= std::max(0, profile.vertical_tolerance_px);
}

bool EnemyCombatCoordinator::active_frame_open(const EnemyAttackProfile& profile, int frame_index) {
    const int start = std::min(profile.active_start_frame, profile.active_end_frame);
    const int end = std::max(profile.active_start_frame, profile.active_end_frame);
    return frame_index >= start && frame_index <= end;
}

int EnemyCombatCoordinator::cooldown_ms_for_outcome(const EnemyAttackProfile& profile,
                                                    EnemyAttackOutcome outcome) {
    switch (outcome) {
    case EnemyAttackOutcome::Started: return std::max(0, profile.cooldown_ms_on_start);
    case EnemyAttackOutcome::Hit: return std::max(0, profile.cooldown_ms_on_hit);
    case EnemyAttackOutcome::Whiff: return std::max(0, profile.cooldown_ms_on_whiff);
    case EnemyAttackOutcome::Interrupted: return std::max(0, profile.cooldown_ms_on_interrupt);
    case EnemyAttackOutcome::RecoveryComplete: return std::max(0, profile.cooldown_ms_on_recovery_complete);
    case EnemyAttackOutcome::None: break;
    }
    return 0;
}

bool EnemyCombatCoordinator::cooldown_ready(const EnemyAttackCooldowns& cooldowns,
                                            const std::string& profile_id,
                                            std::chrono::steady_clock::time_point now) {
    if (profile_id.empty()) {
        return true;
    }
    const auto it = cooldowns.expiry_by_profile.find(profile_id);
    return it == cooldowns.expiry_by_profile.end() || now >= it->second;
}

void EnemyCombatCoordinator::start_cooldown(EnemyAttackCooldowns& cooldowns,
                                            const EnemyAttackProfile& profile,
                                            EnemyAttackOutcome outcome,
                                            std::chrono::steady_clock::time_point now) {
    const int ms = cooldown_ms_for_outcome(profile, outcome);
    if (profile.id.empty() || ms <= 0) {
        return;
    }
    cooldowns.expiry_by_profile[profile.id] = now + std::chrono::milliseconds(ms);
    runtime_stats::FrameStatsRecorder::instance().set("enemy_ai.attack_cooldown_ms", ms);
    runtime_stats::FrameStatsRecorder::instance().set("enemy_ai.attack_cooldown_reason", outcome_name(outcome));
}

std::optional<EnemyAttackCandidate> EnemyCombatCoordinator::select_best_candidate(
    const std::vector<EnemyAttackCandidate>& candidates) {
    if (candidates.empty()) {
        return std::nullopt;
    }
    auto better = [](const EnemyAttackCandidate& candidate, const EnemyAttackCandidate& current) {
        const int candidate_score = static_cast<int>(candidate.window_score);
        const int current_score = static_cast<int>(current.window_score);
        if (candidate_score != current_score) {
            return candidate_score > current_score;
        }
        if (candidate.facing_score != current.facing_score) {
            return candidate.facing_score > current.facing_score;
        }
        if (candidate.target_id != current.target_id) {
            return candidate.target_id < current.target_id;
        }
        if (candidate.path_index != current.path_index) {
            return candidate.path_index < current.path_index;
        }
        return candidate.profile.id < current.profile.id;
    };
    EnemyAttackCandidate best = candidates.front();
    for (const EnemyAttackCandidate& candidate : candidates) {
        if (better(candidate, best)) {
            best = candidate;
        }
    }
    return best;
}

AttackCommitPhase EnemyCombatCoordinator::phase_for_frame(const EnemyAttackProfile& profile,
                                                          int frame_started,
                                                          int current_frame) {
    const int elapsed = std::max(0, current_frame - frame_started);
    if (elapsed < std::max(0, profile.startup_frames)) {
        return AttackCommitPhase::Startup;
    }
    if (active_frame_open(profile, elapsed)) {
        return AttackCommitPhase::Active;
    }
    const int recovery_end = std::max(profile.active_end_frame, profile.active_start_frame) +
                             std::max(0, profile.recovery_frames);
    if (elapsed <= recovery_end) {
        return AttackCommitPhase::Recovery;
    }
    return AttackCommitPhase::Complete;
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

const char* EnemyCombatCoordinator::outcome_name(EnemyAttackOutcome outcome) {
    switch (outcome) {
    case EnemyAttackOutcome::None: return "None";
    case EnemyAttackOutcome::Started: return "Started";
    case EnemyAttackOutcome::Hit: return "Hit";
    case EnemyAttackOutcome::Whiff: return "Whiff";
    case EnemyAttackOutcome::Interrupted: return "Interrupted";
    case EnemyAttackOutcome::RecoveryComplete: return "RecoveryComplete";
    }
    return "Unknown";
}

long long EnemyCombatCoordinator::horizontal_distance_sq(const Asset& self, const Asset& target) {
    return horizontal_distance_sq(axis::WorldPos{self.world_x(), self.world_y(), self.world_z()},
                                  axis::WorldPos{target.world_x(), target.world_y(), target.world_z()});
}

long long EnemyCombatCoordinator::horizontal_distance_sq(const axis::WorldPos& self,
                                                         const axis::WorldPos& target) {
    const long long dx = static_cast<long long>(target.x) - static_cast<long long>(self.x);
    const long long dz = static_cast<long long>(target.z) - static_cast<long long>(self.z);
    return (dx * dx) + (dz * dz);
}

int EnemyCombatCoordinator::vertical_delta_px(const Asset& self, const Asset& target) {
    return target.world_y() - self.world_y();
}

} // namespace animation_update::custom_controllers::internal
