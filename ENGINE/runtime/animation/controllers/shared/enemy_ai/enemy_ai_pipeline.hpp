#pragma once

#include <chrono>
#include <string>

#include "animation/controllers/shared/controller_types.hpp"
#include "animation/controllers/shared/internal/controller_agent_system.hpp"
#include "core/axis_convention.hpp"

class Asset;

namespace animation_update::custom_controllers::enemy_ai {

using BehaviorState = internal::BehaviorState;
using Clock = std::chrono::steady_clock;

struct PerceptionSnapshot {
    Asset* target = nullptr;
    bool target_valid = false;
    bool target_in_same_room = false;
    long long target_distance_sq = 0;
    axis::WorldPos self_position{0, 0, 0};
    axis::WorldPos home_position{0, 0, 0};
    Clock::time_point now{};
};

struct IntentSelection {
    EnemyAgentPhase phase = EnemyAgentPhase::Idle;
    bool target_should_be_committed = false;
    bool entering_attack_window = false;
    bool leaving_attack_window_to_recover = false;
};

struct PositioningRequest {
    EnemyAgentPhase phase = EnemyAgentPhase::Idle;
    bool should_face_target = false;
    bool should_seek_standoff = false;
    bool should_retreat = false;
    bool should_return_home = false;
    int desired_standoff_px = 0;
    int retreat_distance_px = 0;
    int return_home_threshold_px = 0;
};

struct NavigationRequest {
    enum class RouteKind {
        None,
        SeekTarget,
        RetreatFromTarget,
        ReturnHome,
    };

    RouteKind route = RouteKind::None;
    int distance_px = 0;
    MovementConfig movement{};
};

struct LocomotionAnimationRequest {
    bool may_move = false;
    bool movement_allows_attacking = false;
    bool force_attacking_enabled = false;
};

struct AttackCommitment {
    bool committed_to_target = false;
    bool attack_window_active = false;
    bool manual_attack_allowed = false;
};

struct ResultFeedback {
    bool moved = false;
    bool attempted_approach = false;
    bool movement_attack_conflict = false;
    bool forced_return_home_fallback = false;
    int no_progress_frames = 0;
    int attack_window_enter_count = 0;
    int attack_window_exit_count = 0;
    int return_home_fallback_count = 0;
};

struct EnemyAiFrame {
    PerceptionSnapshot perception{};
    IntentSelection intent{};
    PositioningRequest positioning{};
    NavigationRequest navigation{};
    LocomotionAnimationRequest locomotion{};
    AttackCommitment attack{};
    ResultFeedback feedback{};
};

struct EnemyAiPlanConfig {
    EnemyAgentConfig behavior{};
    MovementConfig approach_move{};
    MovementConfig retreat_move{};
};

class EnemyAiPipeline {
public:
    static PerceptionSnapshot perceive(Asset& self,
                                       Asset* target,
                                       bool target_in_same_room,
                                       const BehaviorState& state,
                                       Clock::time_point now = Clock::now());
    static IntentSelection select_intent(const BehaviorState& state,
                                         const PerceptionSnapshot& perception,
                                         const EnemyAgentConfig& config);
    static PositioningRequest choose_positioning(const IntentSelection& intent,
                                                 const EnemyAgentConfig& config);
    static NavigationRequest choose_navigation(const PositioningRequest& positioning,
                                               const EnemyAiPlanConfig& config);
    static LocomotionAnimationRequest choose_locomotion(const NavigationRequest& navigation,
                                                        const EnemyAiPlanConfig& config);
    static AttackCommitment choose_attack_commitment(const IntentSelection& intent);
    static ResultFeedback apply_result_feedback(BehaviorState& state,
                                                const PerceptionSnapshot& perception,
                                                const IntentSelection& intent,
                                                bool moved,
                                                const EnemyAgentConfig& config);
};

class LegacyEnemyAiAdapter {
public:
    static EnemyAiFrame tick(Asset& self,
                             Asset* target,
                             bool target_in_same_room,
                             BehaviorState& state,
                             const EnemyAiPlanConfig& config);
};

const char* phase_name(EnemyAgentPhase phase);
const char* route_name(NavigationRequest::RouteKind route);

} // namespace animation_update::custom_controllers::enemy_ai
