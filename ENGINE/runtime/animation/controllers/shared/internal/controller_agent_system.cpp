#include "animation/controllers/shared/internal/controller_agent_system.hpp"

#include <algorithm>
#include <cmath>

#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace animation_update::custom_controllers::internal {

namespace {

constexpr float kEpsilon = 1e-5f;

int clamp_step(int step_px) {
    return std::max(1, step_px);
}

int resolve_visit_threshold(const MovementConfig& config, int step_px) {
    const int requested = std::max(0, config.visit_threshold_px);
    return std::min(requested, std::max(0, step_px - 1));
}

axis::WorldPos lock_to_ground_if_needed(const Asset& self,
                                        const axis::WorldPos& requested,
                                        const MovementConfig& config) {
    if (config.allow_vertical_movement) {
        return requested;
    }
    Assets* owner_assets = self.get_assets();
    if (!owner_assets) {
        return axis::WorldPos{requested.x, self.world_y(), requested.z};
    }
    const world::GridPoint floor_point =
        owner_assets->resolve_floor_world_point(SDL_Point{requested.x, requested.z}, self.grid_resolution);
    return axis::WorldPos{requested.x, floor_point.world_y(), requested.z};
}

} // namespace

axis::WorldPos ControllerAgentSystem::world_position(const Asset& self) {
    return axis::WorldPos{self.world_x(), self.world_y(), self.world_z()};
}

bool ControllerAgentSystem::auto_move_3d(Asset& self,
                                            const axis::WorldPos& target,
                                            const MovementConfig& config) {
    if (!self.anim_) {
        return false;
    }
    const axis::WorldPos resolved_target = lock_to_ground_if_needed(self, target, config);
    self.anim_->auto_move_3d(resolved_target,
                             std::max(0, config.visit_threshold_px),
                             config.resolution_layer,
                             config.override_non_locked,
                             config.combat_overrides);
    return true;
}

bool ControllerAgentSystem::move_by_delta_3d(Asset& self,
                                                const axis::WorldPos& delta,
                                                const std::string& animation,
                                                bool override_non_locked) {
    if (!self.anim_) {
        return false;
    }
    if (delta.x == 0 && delta.y == 0 && delta.z == 0) {
        return false;
    }
    self.anim_->move_3d(delta, animation, true, override_non_locked);
    return true;
}

bool ControllerAgentSystem::move_toward_point(Asset& self,
                                                 const axis::WorldPos& target,
                                                 int step_px,
                                                 const MovementConfig& config) {
    const axis::WorldPos from = world_position(self);
    const double dx = static_cast<double>(target.x - from.x);
    const double dy = static_cast<double>(target.y - from.y);
    const double dz = static_cast<double>(target.z - from.z);
    const double dist = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    if (dist <= kEpsilon) {
        return false;
    }

    const double step = static_cast<double>(clamp_step(step_px));
    const double t = std::min(1.0, step / dist);
    const axis::WorldPos next{
        static_cast<int>(std::lround(static_cast<double>(from.x) + (dx * t))),
        static_cast<int>(std::lround(static_cast<double>(from.y) + (dy * t))),
        static_cast<int>(std::lround(static_cast<double>(from.z) + (dz * t)))};

    MovementConfig effective = config;
    effective.visit_threshold_px = resolve_visit_threshold(config, clamp_step(step_px));
    return auto_move_3d(self, next, effective);
}

bool ControllerAgentSystem::move_away_from_point(Asset& self,
                                                    const axis::WorldPos& point,
                                                    int step_px,
                                                    const MovementConfig& config) {
    const axis::WorldPos from = world_position(self);
    const double dx = static_cast<double>(from.x - point.x);
    const double dy = static_cast<double>(from.y - point.y);
    const double dz = static_cast<double>(from.z - point.z);
    const double dist = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    if (dist <= kEpsilon) {
        return false;
    }

    const int step = clamp_step(step_px);
    const axis::WorldPos target{
        from.x + static_cast<int>(std::lround((dx / dist) * static_cast<double>(step))),
        from.y + static_cast<int>(std::lround((dy / dist) * static_cast<double>(step))),
        from.z + static_cast<int>(std::lround((dz / dist) * static_cast<double>(step)))};

    MovementConfig effective = config;
    effective.visit_threshold_px = resolve_visit_threshold(config, step);
    return auto_move_3d(self, target, effective);
}

bool ControllerAgentSystem::seek_target(Asset& self,
                                           const Asset& target,
                                           int desired_range_px,
                                           const MovementConfig& config) {
    const long long dist_sq = distance_sq_3d(self, target);
    const long long desired_sq = static_cast<long long>(std::max(0, desired_range_px)) *
                                 static_cast<long long>(std::max(0, desired_range_px));
    if (dist_sq <= desired_sq) {
        return false;
    }

    const double dist = std::sqrt(static_cast<double>(std::max<long long>(1, dist_sq)));
    const int step = std::max(1, static_cast<int>(std::lround(dist - std::max(0, desired_range_px))));
    return move_toward_point(self,
                             axis::WorldPos{target.world_x(), target.world_y(), target.world_z()},
                             step,
                             config);
}

bool ControllerAgentSystem::chase_target(Asset& self,
                                            const Asset& target,
                                            const MovementConfig& config) {
    return seek_target(self, target, 0, config);
}

bool ControllerAgentSystem::retreat_from_target(Asset& self,
                                                   const Asset& target,
                                                   int retreat_distance_px,
                                                   const MovementConfig& config) {
    return move_away_from_point(self,
                                axis::WorldPos{target.world_x(), target.world_y(), target.world_z()},
                                std::max(1, retreat_distance_px),
                                config);
}

bool ControllerAgentSystem::patrol(Asset& self,
                                      const std::vector<axis::WorldPos>& points,
                                      PatrolState& patrol_state,
                                      const MovementConfig& config) {
    if (points.empty()) {
        return false;
    }

    const std::size_t safe_index = patrol_state.next_index % points.size();
    const axis::WorldPos& target = points[safe_index];
    const long long dist_sq = distance_sq_3d(self, target);
    const int visit = std::max(1, config.visit_threshold_px);
    const long long visit_sq = static_cast<long long>(visit) * static_cast<long long>(visit);
    if (dist_sq <= visit_sq) {
        patrol_state.next_index = (safe_index + 1) % points.size();
    }

    const axis::WorldPos& next = points[patrol_state.next_index % points.size()];
    return auto_move_3d(self, next, config);
}

bool ControllerAgentSystem::idle_wander(Asset& self,
                                           std::mt19937& rng,
                                           int min_delta_px,
                                           int max_delta_px,
                                           const MovementConfig& config) {
    const int min_step = std::min(min_delta_px, max_delta_px);
    const int max_step = std::max(min_delta_px, max_delta_px);
    if (max_step <= 0) {
        return false;
    }

    std::uniform_int_distribution<int> dist(min_step, max_step);
    const int dx = dist(rng);
    const int dz = dist(rng);
    const int dy = dist(rng) / 4;
    const axis::WorldPos target{
        self.world_x() + dx,
        config.allow_vertical_movement ? self.world_y() + dy : self.world_y(),
        self.world_z() + dz};
    return auto_move_3d(self, target, config);
}

bool ControllerAgentSystem::return_to_origin(Asset& self,
                                                const axis::WorldPos& origin,
                                                int arrival_threshold_px,
                                                const MovementConfig& config) {
    const long long dist_sq = distance_sq_3d(self, origin);
    const int threshold = std::max(0, arrival_threshold_px);
    const long long threshold_sq = static_cast<long long>(threshold) * static_cast<long long>(threshold);
    if (dist_sq <= threshold_sq) {
        return false;
    }
    return auto_move_3d(self, origin, config);
}

bool ControllerAgentSystem::face_target(Asset& self, const Asset& target) {
    const float dx = static_cast<float>(target.world_x() - self.world_x());
    const float dz = static_cast<float>(target.world_z() - self.world_z());
    return face_direction(self, dx, dz);
}

bool ControllerAgentSystem::face_direction(Asset& self, float dir_x, float dir_z, float pitch_radians) {
    if (!std::isfinite(dir_x) || !std::isfinite(dir_z)) {
        return false;
    }

    const float magnitude_sq = (dir_x * dir_x) + (dir_z * dir_z);
    if (magnitude_sq <= kEpsilon) {
        return false;
    }

    const float heading = std::atan2(dir_z, dir_x);
    bool changed = self.set_directional_heading_radians(heading);
    changed = self.set_directional_pitch_radians(pitch_radians) || changed;
    const float len = std::sqrt(magnitude_sq);
    const float unit_x = dir_x / len;
    const float unit_z = dir_z / len;
    changed = self.set_directional_target_world_xz(static_cast<float>(self.world_x()) + (unit_x * 128.0f),
                                                    static_cast<float>(self.world_z()) + (unit_z * 128.0f)) || changed;
    return changed;
}

long long ControllerAgentSystem::distance_sq_3d(const Asset& a, const Asset& b) {
    return distance_sq_3d(a, axis::WorldPos{b.world_x(), b.world_y(), b.world_z()});
}

long long ControllerAgentSystem::distance_sq_3d(const Asset& a, const axis::WorldPos& b) {
    const long long dx = static_cast<long long>(b.x) - static_cast<long long>(a.world_x());
    const long long dy = static_cast<long long>(b.y) - static_cast<long long>(a.world_y());
    const long long dz = static_cast<long long>(b.z) - static_cast<long long>(a.world_z());
    return (dx * dx) + (dy * dy) + (dz * dz);
}

} // namespace animation_update::custom_controllers::internal




#include <algorithm>
#include <cmath>
#include <string>

#include "assets/asset/Asset.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "utils/log.hpp"

namespace animation_update::custom_controllers::internal {

namespace {

long long distance_sq_3d(const Asset& a, const Asset& b) {
    return ControllerAgentSystem::distance_sq_3d(a, b);
}

long long distance_sq_3d(const Asset& a, const axis::WorldPos& b) {
    return ControllerAgentSystem::distance_sq_3d(a, b);
}

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

} // namespace

void ControllerAgentSystem::ensure_home(BehaviorState& state, const Asset& self) {
    if (state.initialized_home) {
        return;
    }
    state.home = axis::WorldPos{self.world_x(), self.world_y(), self.world_z()};
    state.initialized_home = true;
}

bool ControllerAgentSystem::tick_wander(Asset& self,
                                           Asset* target,
                                           BehaviorState& state,
                                           std::mt19937& rng,
                                           int idle_radius_px,
                                           int min_wander_delta_px,
                                           int max_wander_delta_px,
                                           const MovementConfig& config) {
    ensure_home(state, self);

    if (!target) {
        state.mode = EnemyAgentPhase::Idle;
        return ControllerAgentSystem::idle_wander(self,
                                                     rng,
                                                     min_wander_delta_px,
                                                     max_wander_delta_px,
                                                     config);
    }

    const long long dist_sq = distance_sq_3d(self, *target);
    const long long idle_sq = static_cast<long long>(std::max(0, idle_radius_px)) *
                              static_cast<long long>(std::max(0, idle_radius_px));
    if (dist_sq <= idle_sq) {
        state.mode = EnemyAgentPhase::Idle;
        return false;
    }

    state.mode = EnemyAgentPhase::Acquire;
    return ControllerAgentSystem::idle_wander(self,
                                                 rng,
                                                 min_wander_delta_px,
                                                 max_wander_delta_px,
                                                 config);
}

void ControllerAgentSystem::tick_enemy_behavior(Asset& self,
                                                   Asset* target,
                                                   BehaviorState& state,
                                                   const EnemyAgentConfig& config,
                                                   const MovementConfig& chase_move,
                                                   const MovementConfig& retreat_move) {
    ensure_home(state, self);
    const bool target_valid = target && !target->dead && target->active;
    const int desired_standoff_px = std::max(0, config.ranges.desired_standoff_px);
    const long long target_dist_sq = target_valid ? distance_sq_3d(self, *target) : 0;
    const auto now = std::chrono::steady_clock::now();
    const EnemyPhaseDecision decision =
        decide_enemy_phase(state, target_valid, target_dist_sq, now, config);
    bool moved = false;

    MovementConfig chase_cfg = chase_move;
    chase_cfg.combat_overrides.attacking_enabled = true;
    chase_cfg.combat_overrides.force_attacking_enabled = config.force_attacking_enabled;
    MovementConfig retreat_cfg = retreat_move;
    retreat_cfg.combat_overrides.attacking_enabled = false;

    state.mode = decision.phase;
    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.set("enemy_ai.phase", phase_name(state.mode));
    frame_stats.set("enemy_ai.no_progress_frames", state.no_progress_frames);
    frame_stats.set("enemy_ai.return_home_fallback_count", state.return_home_fallback_count);
    frame_stats.set("enemy_ai.attack_window_enter_count", state.attack_window_enter_count);
    frame_stats.set("enemy_ai.attack_window_exit_count", state.attack_window_exit_count);
    frame_stats.set("enemy_ai.recover_active", decision.phase == EnemyAgentPhase::Recover);
    frame_stats.set("enemy_ai.approach_attempted", false);
    frame_stats.set("enemy_ai.approach_moved", false);
    frame_stats.set("enemy_ai.movement_attack_conflict_flag", false);

    if (decision.target_should_be_committed) {
        self.needs_target = false;
        self.target_reached = true;
    } else {
        self.target_reached = false;
        self.needs_target = true;
    }

    if (decision.phase == EnemyAgentPhase::ReturnHome) {
        moved = tick_return_home(self, state, config.return_home_threshold_px, chase_cfg);
        state.no_progress_frames = moved ? 0 : state.no_progress_frames + 1;
        frame_stats.set("enemy_ai.no_progress_frames", state.no_progress_frames);
        return;
    }

    if (!target_valid) {
        return;
    }

    if (decision.enter_attack_window) {
        ++state.attack_window_enter_count;
        state.attack_window_until =
            now + std::chrono::milliseconds(std::max(1, config.attack_window_ms));
        frame_stats.set("enemy_ai.attack_window_enter_count", state.attack_window_enter_count);
    }
    if (decision.leave_attack_window_to_recover) {
        ++state.attack_window_exit_count;
        state.recover_until = now + std::chrono::milliseconds(std::max(1, config.recover_ms));
        frame_stats.set("enemy_ai.attack_window_exit_count", state.attack_window_exit_count);
    }

    if (decision.phase == EnemyAgentPhase::Recover) {
        moved = ControllerAgentSystem::retreat_from_target(
            self,
            *target,
            std::max(1, config.retreat_distance_px),
            retreat_cfg);
        frame_stats.set("enemy_ai.movement_attack_conflict_flag", decision.target_should_be_committed && moved);
        state.no_progress_frames = moved ? 0 : state.no_progress_frames + 1;
        frame_stats.set("enemy_ai.no_progress_frames", state.no_progress_frames);
        return;
    }
    if (decision.phase == EnemyAgentPhase::Approach) {
        frame_stats.set("enemy_ai.approach_attempted", true);
        moved = ControllerAgentSystem::seek_target(self, *target, desired_standoff_px, chase_cfg);
        frame_stats.set("enemy_ai.approach_moved", moved);
        frame_stats.set("enemy_ai.movement_attack_conflict_flag", decision.target_should_be_committed && moved);
        if (!moved && target_dist_sq > static_cast<long long>(config.ranges.attack_radius_px) *
                                           static_cast<long long>(config.ranges.attack_radius_px)) {
            ++state.no_progress_frames;
        } else {
            state.no_progress_frames = 0;
        }
        frame_stats.set("enemy_ai.no_progress_frames", state.no_progress_frames);
        if (state.no_progress_frames >= 45) {
            const std::string self_name =
                (self.info && !self.info->name.empty()) ? self.info->name : std::string{"<unknown>"};
            vibble::log::warn("[EnemyAI] No progress while approaching target; forcing return-home fallback for '" +
                              self_name + "'");
            ++state.return_home_fallback_count;
            state.mode = EnemyAgentPhase::ReturnHome;
            (void)tick_return_home(self, state, config.return_home_threshold_px, chase_cfg);
            state.no_progress_frames = 0;
            frame_stats.set("enemy_ai.no_progress_frames", state.no_progress_frames);
            frame_stats.set("enemy_ai.return_home_fallback_count", state.return_home_fallback_count);
        }
        return;
    }

    state.no_progress_frames = 0;
    frame_stats.set("enemy_ai.no_progress_frames", state.no_progress_frames);
}

bool ControllerAgentSystem::tick_patrol(Asset& self,
                                           BehaviorState& state,
                                           const std::vector<axis::WorldPos>& patrol_points,
                                           const MovementConfig& config) {
    ensure_home(state, self);
    if (patrol_points.empty()) {
        return false;
    }
    state.mode = EnemyAgentPhase::Patrol;
    return ControllerAgentSystem::patrol(self, patrol_points, state.patrol_state, config);
}

bool ControllerAgentSystem::tick_return_home(Asset& self,
                                                BehaviorState& state,
                                                int arrival_threshold_px,
                                                const MovementConfig& config) {
    ensure_home(state, self);
    state.mode = EnemyAgentPhase::ReturnHome;
    return ControllerAgentSystem::return_to_origin(self,
                                                      state.home,
                                                      std::max(0, arrival_threshold_px),
                                                      config);
}

} // namespace animation_update::custom_controllers::internal

