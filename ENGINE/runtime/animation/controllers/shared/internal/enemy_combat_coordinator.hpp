#pragma once

#include <optional>
#include <string>
#include <vector>

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

struct EnemyAttackProfile {
    std::string id;
    std::string animation_id;
    std::vector<std::string> required_tags;
    std::vector<std::string> excluded_tags;
    int min_range_px = 0;
    int max_range_px = 0;
    int startup_frames = 0;
    int active_start_frame = 0;
    int active_end_frame = 0;
    int recovery_frames = 0;
    int cooldown_ms_on_start = 0;
    int cooldown_ms_on_whiff = 0;
    int cooldown_ms_on_hit = 0;
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

class EnemyCombatCoordinator {
public:
    static EnemyAttackProfile make_legacy_profile(const std::string& id,
                                                  float cooldown_seconds,
                                                  int range_px,
                                                  const std::string& animation_id,
                                                  const std::vector<std::string>& required_tags = {},
                                                  const std::vector<std::string>& excluded_tags = {});

    static AttackRequestResult commit_startup(Asset& self,
                                              Asset& target,
                                              const EnemyAttackProfile& profile);

    static const char* phase_name(AttackCommitPhase phase);
    static long long horizontal_distance_sq(const Asset& self, const Asset& target);
    static int vertical_delta_px(const Asset& self, const Asset& target);
};

} // namespace animation_update::custom_controllers::internal
