#include <cassert>
#include <chrono>

#include "animation/controllers/shared/internal/controller_behavior_system.hpp"

int main() {
    using System = animation_update::custom_controllers::internal::ControllerBehaviorSystem;
    using Phase = animation_update::custom_controllers::EnemyAgentPhase;
    using Config = animation_update::custom_controllers::EnemyAgentConfig;
    using State = animation_update::custom_controllers::internal::BehaviorState;

    const auto now = std::chrono::steady_clock::now();

    // small_spider-like config should reach AttackWindow from approach range.
    {
        Config cfg{};
        cfg.kamikaze = false;
        cfg.ranges.aggro_radius_px = 120;
        cfg.ranges.desired_standoff_px = 3;
        cfg.ranges.attack_radius_px = 16;
        cfg.recover_ms = 210;
        cfg.attack_window_ms = 160;

        State state{};
        const long long approach_sq = 40LL * 40LL;
        const long long attack_sq = 8LL * 8LL;

        const auto d_approach = System::decide_enemy_phase(state, true, approach_sq, now, cfg);
        assert(d_approach.phase == Phase::Approach);

        const auto d_attack = System::decide_enemy_phase(state, true, attack_sq, now, cfg);
        assert(d_attack.phase == Phase::AttackWindow);
        assert(d_attack.target_should_be_committed);
        assert(d_attack.enter_attack_window);
    }

    // bomb-like (kamikaze) config should stay attack-committed and never force recover transition.
    {
        Config cfg{};
        cfg.kamikaze = true;
        cfg.ranges.aggro_radius_px = 120;
        cfg.ranges.desired_standoff_px = 0;
        cfg.ranges.attack_radius_px = 80;
        cfg.recover_ms = 0;
        cfg.attack_window_ms = 200;

        State state{};
        state.mode = Phase::AttackWindow;
        state.attack_window_until = now - std::chrono::milliseconds(1);

        const long long attack_sq = 30LL * 30LL;
        const auto d = System::decide_enemy_phase(state, true, attack_sq, now, cfg);
        assert(d.phase == Phase::AttackWindow);
        assert(d.target_should_be_committed);
        assert(!d.leave_attack_window_to_recover);
    }
}

