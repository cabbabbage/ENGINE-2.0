#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "animation/attack_validation.hpp"
#include "core/axis_convention.hpp"

class Asset;

namespace animation_update::custom_controllers::internal {

enum class AttackCommitPhase {
    None,
    Startup,
    Active,
    Recovery,
    Complete,
    Interrupted,
};

enum class EnemyAttackOutcome {
    None,
    Started,
    Hit,
    Whiff,
    Interrupted,
    RecoveryComplete,
};

struct EnemyAttackProfile {
    std::string id;
    std::string animation_id;
    std::vector<std::string> required_tags;
    std::vector<std::string> excluded_tags;
    int min_range_px = 0;
    int max_range_px = 0;
    int vertical_tolerance_px = 48;
    int startup_frames = 0;
    int active_start_frame = 0;
    int active_end_frame = 0;
    int recovery_frames = 0;
    int cooldown_ms_on_start = 0;
    int cooldown_ms_on_whiff = 0;
    int cooldown_ms_on_hit = 0;
    int cooldown_ms_on_interrupt = 0;
    int cooldown_ms_on_recovery_complete = 0;
    int prediction_horizon_frames = 0;
    int prediction_padding_px = 0;
    bool requires_facing = true;
    float facing_cone_degrees = 120.0f;
    bool can_track_during_startup = false;
    bool can_retarget_during_startup = false;
    bool allow_contact_fallback = false;
};

struct AttackCommitState {
    AttackCommitPhase phase = AttackCommitPhase::None;
    std::string attacker_id;
    std::string target_id;
    std::string profile_id;
    std::string animation_id;
    std::size_t path_index = 0;
    int frame_started = 0;
    bool hit_dispatched = false;
    bool whiffed = false;
};

struct AttackRequestResult {
    bool accepted = false;
    std::string reason;
    EnemyAttackProfile profile;
    AttackCommitState state;
};

struct EnemyAttackCandidate {
    EnemyAttackProfile profile;
    animation_update::AttackValidation::AttackWindowScore window_score =
        animation_update::AttackValidation::AttackWindowScore::Miss;
    int facing_score = 0;
    std::string target_id;
    std::size_t path_index = 0;
};

struct EnemyAttackCooldowns {
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> expiry_by_profile;
};

class EnemyCombatCoordinator {
public:
    static EnemyAttackProfile make_legacy_profile(const std::string& id,
                                                  float cooldown_seconds,
                                                  int range_px,
                                                  const std::string& animation_id,
                                                  const std::vector<std::string>& required_tags = {},
                                                  const std::vector<std::string>& excluded_tags = {});

    static EnemyAttackProfile make_contact_hazard_profile(const std::string& id,
                                                          int cooldown_frames,
                                                          int contact_range_px);

    static EnemyAttackProfile make_explosion_profile(const std::string& id,
                                                     int arming_frames,
                                                     int active_frames,
                                                     int explosion_radius_px,
                                                     int damage);

    static AttackRequestResult commit_startup(Asset& self,
                                              Asset& target,
                                              const EnemyAttackProfile& profile);

    static bool profile_in_range(const EnemyAttackProfile& profile,
                                 const Asset& self,
                                 const Asset& target);

    static bool active_frame_open(const EnemyAttackProfile& profile, int frame_index);

    static int cooldown_ms_for_outcome(const EnemyAttackProfile& profile,
                                       EnemyAttackOutcome outcome);

    static bool cooldown_ready(const EnemyAttackCooldowns& cooldowns,
                               const std::string& profile_id,
                               std::chrono::steady_clock::time_point now);

    static void start_cooldown(EnemyAttackCooldowns& cooldowns,
                               const EnemyAttackProfile& profile,
                               EnemyAttackOutcome outcome,
                               std::chrono::steady_clock::time_point now);

    static std::optional<EnemyAttackCandidate> select_best_candidate(
        const std::vector<EnemyAttackCandidate>& candidates);

    static AttackCommitPhase phase_for_frame(const EnemyAttackProfile& profile,
                                             int frame_started,
                                             int current_frame);

    static const char* phase_name(AttackCommitPhase phase);
    static const char* outcome_name(EnemyAttackOutcome outcome);
    static long long horizontal_distance_sq(const Asset& self, const Asset& target);
    static long long horizontal_distance_sq(const axis::WorldPos& self, const axis::WorldPos& target);
    static int vertical_delta_px(const Asset& self, const Asset& target);
};

} // namespace animation_update::custom_controllers::internal
