#pragma once

#include <optional>
#include <string>

#include "animation/controllers/shared/controller_types.hpp"
#include "animation/controllers/shared/internal/enemy_perception_system.hpp"
#include "core/axis_convention.hpp"

namespace animation_update::custom_controllers::internal {

enum class EnemyIntentKind {
    Idle,
    Patrol,
    AcquireTarget,
    Pursue,
    HoldRange,
    Reposition,
    AttackCommit,
    Recover,
    Retreat,
    ReturnHome,
    Stunned,
    Dead,
};

struct EnemyIntent {
    EnemyIntentKind kind = EnemyIntentKind::Idle;
    std::optional<std::string> target_id = std::nullopt;
    axis::WorldPos desired_position{0, 0, 0};
    int desired_range_px = 0;
    int min_duration_ms = 0;
    int max_duration_ms = 0;
    bool movement_allowed = false;
    bool attack_allowed = false;
    bool can_interrupt = true;
    std::string reason;
};

class EnemyIntentSystem {
public:
    static EnemyIntent from_legacy_phase(EnemyAgentPhase phase,
                                         const EnemyPerceptionSnapshot& perception,
                                         const EnemyAgentConfig& config);
    static const char* intent_name(EnemyIntentKind kind);
};

} // namespace animation_update::custom_controllers::internal
