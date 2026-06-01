#include "animation/controllers/shared/enemy_ai/enemy_ai_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <string>

#include "assets/asset/Asset.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "utils/log.hpp"

namespace animation_update::custom_controllers::enemy_ai {

namespace {

void ensure_home(BehaviorState& state, const Asset& self) {
    if (state.initialized_home) {
        return;
    }
    state.home = axis::WorldPos{self.world_x(), self.world_y(), self.world_z()};
    state.initialized_home = true;
}

bool target_is_valid(const Asset* target, bool same_room) {
    return target && same_room && !target->dead && target->active;
}

} // namespace

const char* phase_name(EnemyAgentPhase phase) {
    switch (phase) {
    case EnemyAgentPhase::Idle: return "Idle";
    case EnemyAgentPhase::Acquire: return "Acquire";
    case EnemyAgentPhase::Approach: return "Approach";
    case EnemyAgentPhase::AttackWindow: return "AttackWindow";
    case EnemyAgentPhase::Recover: return "Recover";
    case EnemyAgentPhase::ReturnHome: return "ReturnHome";
    case EnemyAgentPhase::Patrol: return "Patrol";
    case EnemyAgentPhase::Custom: return "Custom";
    }
    return "Unknown";
}

const char* route_name(NavigationRequest::RouteKind route) {
    switch (route) {
    case NavigationRequest::RouteKind::None: return "None";
    case NavigationRequest::RouteKind::SeekTarget: return "SeekTarget";
    case NavigationRequest::RouteKind::RetreatFromTarget: return "RetreatFromTarget";
    case NavigationRequest::RouteKind::ReturnHome: return "ReturnHome";
    }
    return "Unknown";
}

PerceptionSnapshot EnemyAiPipeline::perceive(Asset& self,
                                             Asset* target,
                                             bool target_in_same_room,
                                             const BehaviorState& state,
                                             Clock::time_point now) {
    PerceptionSnapshot perception{};
    perception.target = target;
    perception.target_in_same_room = target_in_same_room;
    perception.target_valid = target_is_valid(target, target_in_same_room);
    perception.self_position = axis::WorldPos{self.world_x(), self.world_y(), self.world_z()};
    perception.home_position = state.home;
    perception.now = now;
    if (perception.target_valid) {
        perception.target_distance_sq = internal::ControllerAgentSystem::distance_sq_3d(self, *target);
    }
    return perception;
}

IntentSelection EnemyAiPipeline::select_intent(const BehaviorState& state,
                                               const PerceptionSnapshot& perception,
                                               const EnemyAgentConfig& config) {
    const auto decision = internal::ControllerAgentSystem::decide_enemy_phase(
        state,
        perception.target_valid,
        perception.target_distance_sq,
        perception.now,
        config);

    IntentSelection intent{};
    intent.phase = decision.phase;
    intent.target_should_be_committed = decision.target_should_be_committed;
    intent.entering_attack_window = decision.enter_attack_window;
    intent.leaving_attack_window_to_recover = decision.leave_attack_window_to_recover;
    return intent;
}

PositioningRequest EnemyAiPipeline::choose_positioning(const IntentSelection& intent,
                                                       const EnemyAgentConfig& config) {
    PositioningRequest positioning{};
    positioning.phase = intent.phase;
    positioning.should_face_target = intent.phase == EnemyAgentPhase::AttackWindow;
    positioning.should_seek_standoff = intent.phase == EnemyAgentPhase::Approach;
    positioning.should_retreat = intent.phase == EnemyAgentPhase::Recover;
    positioning.should_return_home = intent.phase == EnemyAgentPhase::ReturnHome;
    positioning.desired_standoff_px = std::max(0, config.ranges.desired_standoff_px);
    positioning.retreat_distance_px = std::max(1, config.retreat_distance_px);
    positioning.return_home_threshold_px = std::max(0, config.return_home_threshold_px);
    return positioning;
}

NavigationRequest EnemyAiPipeline::choose_navigation(const PositioningRequest& positioning,
                                                     const EnemyAiPlanConfig& config) {
    NavigationRequest navigation{};
    if (positioning.should_seek_standoff) {
        navigation.route = NavigationRequest::RouteKind::SeekTarget;
        navigation.distance_px = positioning.desired_standoff_px;
        navigation.movement = config.approach_move;
        navigation.movement.combat_overrides.attacking_enabled = true;
        navigation.movement.combat_overrides.force_attacking_enabled =
            config.behavior.force_attacking_enabled;
        return navigation;
    }
    if (positioning.should_retreat) {
        navigation.route = NavigationRequest::RouteKind::RetreatFromTarget;
        navigation.distance_px = positioning.retreat_distance_px;
        navigation.movement = config.retreat_move;
        navigation.movement.combat_overrides.attacking_enabled = false;
        return navigation;
    }
    if (positioning.should_return_home) {
        navigation.route = NavigationRequest::RouteKind::ReturnHome;
        navigation.distance_px = positioning.return_home_threshold_px;
        navigation.movement = config.approach_move;
        navigation.movement.combat_overrides.attacking_enabled = true;
        navigation.movement.combat_overrides.force_attacking_enabled =
            config.behavior.force_attacking_enabled;
        return navigation;
    }
    return navigation;
}

LocomotionAnimationRequest EnemyAiPipeline::choose_locomotion(const NavigationRequest& navigation,
                                                              const EnemyAiPlanConfig&) {
    LocomotionAnimationRequest locomotion{};
    locomotion.may_move = navigation.route != NavigationRequest::RouteKind::None;
    locomotion.movement_allows_attacking = navigation.movement.combat_overrides.attacking_enabled;
    locomotion.force_attacking_enabled = navigation.movement.combat_overrides.force_attacking_enabled;
    return locomotion;
}

AttackCommitment EnemyAiPipeline::choose_attack_commitment(const IntentSelection& intent) {
    AttackCommitment attack{};
    attack.committed_to_target = intent.target_should_be_committed;
    attack.attack_window_active = intent.phase == EnemyAgentPhase::AttackWindow;
    attack.manual_attack_allowed = attack.attack_window_active && attack.committed_to_target;
    return attack;
}

ResultFeedback EnemyAiPipeline::apply_result_feedback(BehaviorState& state,
                                                      const PerceptionSnapshot& perception,
                                                      const IntentSelection& intent,
                                                      bool moved,
                                                      const EnemyAgentConfig& config) {
    ResultFeedback feedback{};
    feedback.moved = moved;
    feedback.attempted_approach = intent.phase == EnemyAgentPhase::Approach;
    feedback.movement_attack_conflict = intent.target_should_be_committed && moved;

    if (intent.entering_attack_window) {
        ++state.attack_window_enter_count;
        state.attack_window_until =
            perception.now + std::chrono::milliseconds(std::max(1, config.attack_window_ms));
    }
    if (intent.leaving_attack_window_to_recover) {
        ++state.attack_window_exit_count;
        state.recover_until =
            perception.now + std::chrono::milliseconds(std::max(1, config.recover_ms));
    }

    if (intent.phase == EnemyAgentPhase::Approach) {
        const long long attack_radius_sq = static_cast<long long>(config.ranges.attack_radius_px) *
                                          static_cast<long long>(config.ranges.attack_radius_px);
        if (!moved && perception.target_distance_sq > attack_radius_sq) {
            ++state.no_progress_frames;
        } else {
            state.no_progress_frames = 0;
        }
        if (state.no_progress_frames >= 45) {
            ++state.return_home_fallback_count;
            state.mode = EnemyAgentPhase::ReturnHome;
            state.no_progress_frames = 0;
            feedback.forced_return_home_fallback = true;
        }
    } else if (intent.phase == EnemyAgentPhase::Recover || intent.phase == EnemyAgentPhase::ReturnHome) {
        state.no_progress_frames = moved ? 0 : state.no_progress_frames + 1;
    } else {
        state.no_progress_frames = 0;
    }

    feedback.no_progress_frames = state.no_progress_frames;
    feedback.attack_window_enter_count = state.attack_window_enter_count;
    feedback.attack_window_exit_count = state.attack_window_exit_count;
    feedback.return_home_fallback_count = state.return_home_fallback_count;
    return feedback;
}

EnemyAiFrame LegacyEnemyAiAdapter::tick(Asset& self,
                                        Asset* target,
                                        bool target_in_same_room,
                                        BehaviorState& state,
                                        const EnemyAiPlanConfig& config) {
    ensure_home(state, self);

    EnemyAiFrame frame{};
    frame.perception = EnemyAiPipeline::perceive(self, target, target_in_same_room, state);
    frame.intent = EnemyAiPipeline::select_intent(state, frame.perception, config.behavior);
    frame.positioning = EnemyAiPipeline::choose_positioning(frame.intent, config.behavior);
    frame.navigation = EnemyAiPipeline::choose_navigation(frame.positioning, config);
    frame.locomotion = EnemyAiPipeline::choose_locomotion(frame.navigation, config);
    frame.attack = EnemyAiPipeline::choose_attack_commitment(frame.intent);

    state.mode = frame.intent.phase;
    self.needs_target = !frame.attack.committed_to_target;
    self.target_reached = frame.attack.committed_to_target;

    const bool debug_logging = self.anim_ && self.anim_->debug_enabled();
    const std::string self_name =
        (self.info && !self.info->name.empty()) ? self.info->name : std::string{"<unknown>"};

    bool moved = false;
    switch (frame.navigation.route) {
    case NavigationRequest::RouteKind::SeekTarget:
        if (frame.perception.target_valid && frame.perception.target) {
            moved = internal::ControllerAgentSystem::seek_target(
                self,
                *frame.perception.target,
                frame.navigation.distance_px,
                frame.navigation.movement);
        }
        break;
    case NavigationRequest::RouteKind::RetreatFromTarget:
        if (frame.perception.target_valid && frame.perception.target) {
            moved = internal::ControllerAgentSystem::retreat_from_target(
                self,
                *frame.perception.target,
                frame.navigation.distance_px,
                frame.navigation.movement);
        }
        break;
    case NavigationRequest::RouteKind::ReturnHome:
        moved = internal::ControllerAgentSystem::tick_return_home(
            self,
            state,
            frame.navigation.distance_px,
            frame.navigation.movement);
        break;
    case NavigationRequest::RouteKind::None:
        break;
    }

    frame.feedback = EnemyAiPipeline::apply_result_feedback(
        state,
        frame.perception,
        frame.intent,
        moved,
        config.behavior);

    if (frame.feedback.forced_return_home_fallback) {
        vibble::log::warn("[EnemyAI] No progress while approaching target; forcing return-home fallback for '" +
                          self_name + "'");
        (void)internal::ControllerAgentSystem::tick_return_home(
            self,
            state,
            std::max(0, config.behavior.return_home_threshold_px),
            frame.navigation.movement);
    }

    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.set("enemy_ai.phase", phase_name(state.mode));
    frame_stats.set("enemy_ai.pipeline.route", route_name(frame.navigation.route));
    frame_stats.set("enemy_ai.pipeline.manual_attack_allowed", frame.attack.manual_attack_allowed);
    frame_stats.set("enemy_ai.no_progress_frames", frame.feedback.no_progress_frames);
    frame_stats.set("enemy_ai.return_home_fallback_count", frame.feedback.return_home_fallback_count);
    frame_stats.set("enemy_ai.attack_window_enter_count", frame.feedback.attack_window_enter_count);
    frame_stats.set("enemy_ai.attack_window_exit_count", frame.feedback.attack_window_exit_count);
    frame_stats.set("enemy_ai.recover_active", state.mode == EnemyAgentPhase::Recover);
    frame_stats.set("enemy_ai.approach_attempted", frame.feedback.attempted_approach);
    frame_stats.set("enemy_ai.approach_moved", frame.feedback.attempted_approach && frame.feedback.moved);
    frame_stats.set("enemy_ai.movement_attack_conflict_flag", frame.feedback.movement_attack_conflict);

    if (debug_logging) {
        vibble::log::info("[AICombat] '" + self_name + "' pipeline phase=" + phase_name(state.mode) +
                          " route=" + route_name(frame.navigation.route) +
                          " moved=" + std::to_string(frame.feedback.moved));
    }

    return frame;
}

} // namespace animation_update::custom_controllers::enemy_ai
