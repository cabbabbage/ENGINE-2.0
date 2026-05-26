#pragma once

#include <chrono>

#include "animation/animation_update.hpp"

class Asset;

namespace animation_update::custom_controllers {

class EnemyCombatSteering;

enum class EnemyAutoCombatMode {
    SkirmisherShortEvade,
    KamikazeDetonate,
};

struct EnemyAutoCombatConfig {
    EnemyAutoCombatMode mode = EnemyAutoCombatMode::SkirmisherShortEvade;
    int approach_range_px = 220;
    int approach_visit_threshold_px = 90;
    int evade_distance_px = 320;
    int evade_visit_threshold_px = 120;
    int evade_duration_ms = 500;
    bool force_attacking_enabled = false;
};

class EnemyAutoCombatBehavior {
public:
    explicit EnemyAutoCombatBehavior(EnemyAutoCombatConfig config = {});

    void reset();
    void tick(Asset& self, Asset& target, EnemyCombatSteering& steering);

private:
    enum class State {
        Approach,
        Evade,
        Detonating,
    };

    EnemyAutoCombatConfig config_{};
    State state_ = State::Approach;
    std::chrono::steady_clock::time_point evade_until_{};
    bool was_in_attack_animation_ = false;
};

} // namespace animation_update::custom_controllers

