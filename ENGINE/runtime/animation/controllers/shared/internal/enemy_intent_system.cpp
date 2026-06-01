#include "animation/controllers/shared/internal/enemy_intent_system.hpp"

#include <algorithm>

namespace animation_update::custom_controllers::internal {

EnemyIntent EnemyIntentSystem::from_legacy_phase(EnemyAgentPhase phase,
                                                 const EnemyPerceptionSnapshot& perception,
                                                 const EnemyAgentConfig& config) {
    EnemyIntent intent{};
    intent.target_id = perception.target_id;
    intent.desired_position = perception.target_position;
    intent.desired_range_px = std::max(0, config.ranges.desired_standoff_px);
    intent.min_duration_ms = 80;
    intent.max_duration_ms = std::max(config.attack_window_ms, config.recover_ms);

    switch (phase) {
    case EnemyAgentPhase::Idle:
        intent.kind = EnemyIntentKind::Idle;
        intent.reason = "legacy_idle";
        break;
    case EnemyAgentPhase::Acquire:
        intent.kind = EnemyIntentKind::AcquireTarget;
        intent.movement_allowed = true;
        intent.reason = "legacy_acquire";
        break;
    case EnemyAgentPhase::Approach:
        intent.kind = EnemyIntentKind::Pursue;
        intent.movement_allowed = true;
        intent.reason = "legacy_approach";
        break;
    case EnemyAgentPhase::AttackWindow:
        intent.kind = EnemyIntentKind::AttackCommit;
        intent.attack_allowed = true;
        intent.can_interrupt = false;
        intent.min_duration_ms = std::max(1, config.attack_window_ms);
        intent.reason = "legacy_attack_window";
        break;
    case EnemyAgentPhase::Recover:
        intent.kind = EnemyIntentKind::Recover;
        intent.movement_allowed = true;
        intent.can_interrupt = false;
        intent.min_duration_ms = std::max(1, config.recover_ms);
        intent.reason = "legacy_recover";
        break;
    case EnemyAgentPhase::ReturnHome:
        intent.kind = EnemyIntentKind::ReturnHome;
        intent.movement_allowed = true;
        intent.target_id = std::nullopt;
        intent.reason = "legacy_return_home";
        break;
    case EnemyAgentPhase::Patrol:
        intent.kind = EnemyIntentKind::Patrol;
        intent.movement_allowed = true;
        intent.reason = "legacy_patrol";
        break;
    case EnemyAgentPhase::Custom:
        intent.kind = EnemyIntentKind::Reposition;
        intent.movement_allowed = true;
        intent.reason = "legacy_custom";
        break;
    }
    return intent;
}

const char* EnemyIntentSystem::intent_name(EnemyIntentKind kind) {
    switch (kind) {
    case EnemyIntentKind::Idle: return "Idle";
    case EnemyIntentKind::Patrol: return "Patrol";
    case EnemyIntentKind::AcquireTarget: return "AcquireTarget";
    case EnemyIntentKind::Pursue: return "Pursue";
    case EnemyIntentKind::HoldRange: return "HoldRange";
    case EnemyIntentKind::Reposition: return "Reposition";
    case EnemyIntentKind::AttackCommit: return "AttackCommit";
    case EnemyIntentKind::Recover: return "Recover";
    case EnemyIntentKind::Retreat: return "Retreat";
    case EnemyIntentKind::ReturnHome: return "ReturnHome";
    case EnemyIntentKind::Stunned: return "Stunned";
    case EnemyIntentKind::Dead: return "Dead";
    }
    return "Unknown";
}

} // namespace animation_update::custom_controllers::internal
