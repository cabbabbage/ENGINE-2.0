#pragma once

#include <SDL3/SDL.h>

#include <chrono>

#include "animation/animation_update.hpp"

class Asset;

namespace animation_update::custom_controllers {

struct EnemyCombatSteeringConfig {
    int retarget_ms = 180;
    int target_repath_px = 96;
    int self_progress_px = 10;
    int stuck_frame_limit = 18;
    int detour_px = 180;
    int max_plan_distance_px = 900;
};

class EnemyCombatSteering {
public:
    explicit EnemyCombatSteering(EnemyCombatSteeringConfig config = {});

    void reset();
    void tick_progress(const Asset& self);
    bool is_stuck() const;

    bool approach(Asset& self,
                  const Asset& target,
                  int desired_range_px,
                  int visit_threshold_px,
                  bool force_replan,
                  AnimationUpdate::AutoMoveCombatOverrides combat_overrides = {});

    bool evade(Asset& self,
               const Asset& target,
               int evade_distance_px,
               int visit_threshold_px,
               bool force_replan,
               AnimationUpdate::AutoMoveCombatOverrides combat_overrides = {});

private:
    bool should_replan(const Asset& self, const Asset& target, bool force_replan) const;
    void mark_planned(const Asset& self, const Asset& target);
    SDL_Point bounded_target_from_delta(const Asset& self, SDL_Point delta) const;

    EnemyCombatSteeringConfig config_{};
    std::chrono::steady_clock::time_point next_plan_time_{};
    SDL_Point last_self_pos_{0, 0};
    SDL_Point last_target_pos_{0, 0};
    bool has_last_self_pos_ = false;
    bool has_last_target_pos_ = false;
    int stagnant_frames_ = 0;
    int detour_side_ = 1;
};

long long distance_sq_xz(const Asset& a, const Asset& b);
bool current_animation_has_tag(const Asset& self, const char* tag);

} // namespace animation_update::custom_controllers
