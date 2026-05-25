#include "animation/controllers/shared/enemy_combat_steering.hpp"

#include <algorithm>
#include <cmath>

#include "animation/animation_tag_utils.hpp"
#include "assets/asset/Asset.hpp"

namespace animation_update::custom_controllers {

namespace {

int distance_sq(SDL_Point a, SDL_Point b) {
    const int dx = b.x - a.x;
    const int dz = b.y - a.y;
    return dx * dx + dz * dz;
}

SDL_Point normalized_scaled_delta(double dx, double dz, int length_px) {
    const double len = std::sqrt(std::max(1.0, dx * dx + dz * dz));
    return SDL_Point{
        static_cast<int>(std::lround((dx / len) * static_cast<double>(length_px))),
        static_cast<int>(std::lround((dz / len) * static_cast<double>(length_px)))
    };
}

} // namespace

EnemyCombatSteering::EnemyCombatSteering(EnemyCombatSteeringConfig config)
    : config_(config) {}

void EnemyCombatSteering::reset() {
    next_plan_time_ = {};
    last_self_pos_ = SDL_Point{0, 0};
    last_target_pos_ = SDL_Point{0, 0};
    has_last_self_pos_ = false;
    has_last_target_pos_ = false;
    stagnant_frames_ = 0;
    detour_side_ = -detour_side_;
    if (detour_side_ == 0) {
        detour_side_ = 1;
    }
}

void EnemyCombatSteering::tick_progress(const Asset& self) {
    const SDL_Point current = self.world_xz_point();
    if (!has_last_self_pos_) {
        last_self_pos_ = current;
        has_last_self_pos_ = true;
        stagnant_frames_ = 0;
        return;
    }

    const int progress_sq = config_.self_progress_px * config_.self_progress_px;
    if (distance_sq(current, last_self_pos_) >= progress_sq) {
        last_self_pos_ = current;
        stagnant_frames_ = 0;
        return;
    }

    if (!self.needs_target && !self.target_reached) {
        ++stagnant_frames_;
    } else {
        stagnant_frames_ = 0;
    }
}

bool EnemyCombatSteering::is_stuck() const {
    return stagnant_frames_ >= config_.stuck_frame_limit;
}

bool EnemyCombatSteering::should_replan(const Asset& self, const Asset& target, bool force_replan) const {
    if (force_replan || self.needs_target || self.target_reached || is_stuck()) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (next_plan_time_.time_since_epoch().count() != 0 && now < next_plan_time_) {
        return false;
    }

    if (!has_last_target_pos_) {
        return true;
    }

    const int repath_sq = config_.target_repath_px * config_.target_repath_px;
    return distance_sq(target.world_xz_point(), last_target_pos_) >= repath_sq;
}

void EnemyCombatSteering::mark_planned(const Asset& self, const Asset& target) {
    last_self_pos_ = self.world_xz_point();
    last_target_pos_ = target.world_xz_point();
    has_last_self_pos_ = true;
    has_last_target_pos_ = true;
    stagnant_frames_ = 0;
    next_plan_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, config_.retarget_ms));
}

SDL_Point EnemyCombatSteering::bounded_target_from_delta(const Asset& self, SDL_Point delta) const {
    const double dist = std::sqrt(static_cast<double>(delta.x) * delta.x +
                                  static_cast<double>(delta.y) * delta.y);
    if (dist > static_cast<double>(config_.max_plan_distance_px) && dist > 1.0) {
        const double scale = static_cast<double>(config_.max_plan_distance_px) / dist;
        delta.x = static_cast<int>(std::lround(static_cast<double>(delta.x) * scale));
        delta.y = static_cast<int>(std::lround(static_cast<double>(delta.y) * scale));
    }
    return SDL_Point{self.world_x() + delta.x, self.world_z() + delta.y};
}

bool EnemyCombatSteering::approach(Asset& self,
                                   const Asset& target,
                                   int desired_range_px,
                                   int visit_threshold_px,
                                   bool force_replan,
                                   AnimationUpdate::AutoMoveCombatOverrides combat_overrides) {
    if (!self.anim_ || !should_replan(self, target, force_replan)) {
        return false;
    }

    const double to_target_x = static_cast<double>(target.world_x() - self.world_x());
    const double to_target_z = static_cast<double>(target.world_z() - self.world_z());
    const double dist = std::sqrt(std::max(1.0, to_target_x * to_target_x + to_target_z * to_target_z));
    int step_px = static_cast<int>(std::lround(dist - static_cast<double>(std::max(0, desired_range_px))));
    step_px = std::clamp(step_px, 1, std::max(1, config_.max_plan_distance_px));

    SDL_Point delta = normalized_scaled_delta(to_target_x, to_target_z, step_px);
    if (is_stuck()) {
        const SDL_Point tangent{
            static_cast<int>(std::lround((-to_target_z / dist) * static_cast<double>(config_.detour_px) * detour_side_)),
            static_cast<int>(std::lround((to_target_x / dist) * static_cast<double>(config_.detour_px) * detour_side_))
        };
        delta.x += tangent.x;
        delta.y += tangent.y;
        detour_side_ = -detour_side_;
    }

    const SDL_Point world_target = bounded_target_from_delta(self, delta);
    self.anim_->auto_move(world_target, std::max(0, visit_threshold_px), std::nullopt, true, combat_overrides);
    mark_planned(self, target);
    return true;
}

bool EnemyCombatSteering::evade(Asset& self,
                                const Asset& target,
                                int evade_distance_px,
                                int visit_threshold_px,
                                bool force_replan,
                                AnimationUpdate::AutoMoveCombatOverrides combat_overrides) {
    if (!self.anim_ || !should_replan(self, target, force_replan)) {
        return false;
    }

    const double away_x = static_cast<double>(self.world_x() - target.world_x());
    const double away_z = static_cast<double>(self.world_z() - target.world_z());
    const double len = std::sqrt(std::max(1.0, away_x * away_x + away_z * away_z));
    SDL_Point delta = normalized_scaled_delta(away_x, away_z, std::max(1, evade_distance_px));
    const SDL_Point tangent{
        static_cast<int>(std::lround((-away_z / len) * static_cast<double>(evade_distance_px / 2) * detour_side_)),
        static_cast<int>(std::lround((away_x / len) * static_cast<double>(evade_distance_px / 2) * detour_side_))
    };
    delta.x += tangent.x;
    delta.y += tangent.y;
    detour_side_ = -detour_side_;

    const SDL_Point world_target = bounded_target_from_delta(self, delta);
    self.anim_->auto_move(world_target, std::max(0, visit_threshold_px), std::nullopt, true, combat_overrides);
    mark_planned(self, target);
    return true;
}

long long distance_sq_xz(const Asset& a, const Asset& b) {
    const long long dx = static_cast<long long>(b.world_x()) - static_cast<long long>(a.world_x());
    const long long dz = static_cast<long long>(b.world_z()) - static_cast<long long>(a.world_z());
    return dx * dx + dz * dz;
}

bool current_animation_has_tag(const Asset& self, const char* tag) {
    if (!self.info) {
        return false;
    }
    const auto it = self.info->animations.find(self.current_animation);
    if (it == self.info->animations.end()) {
        return false;
    }
    return animation_update::tag_utils::has_normalized_tag(it->second.tags, tag);
}

} // namespace animation_update::custom_controllers
