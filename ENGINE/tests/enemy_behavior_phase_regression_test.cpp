#include <cassert>
#include <chrono>

#include "animation/controllers/shared/internal/controller_behavior_system.hpp"

int main() {
    using System = animation_update::custom_controllers::internal::ControllerBehaviorSystem;
    using Phase = animation_update::custom_controllers::EnemyAgentPhase;
    using Config = animation_update::custom_controllers::EnemyAgentConfig;
    using State = animation_update::custom_controllers::internal::BehaviorState;

    Config cfg{};
    cfg.ranges.aggro_radius_px = 200;
    cfg.ranges.attack_radius_px = 60;
    cfg.attack_window_ms = 120;
    cfg.recover_ms = 300;
    cfg.kamikaze = false;

    const auto now = std::chrono::steady_clock::now();
    const long long outside_aggro_sq = 250LL * 250LL;
    const long long approach_sq = 150LL * 150LL;
    const long long attack_sq = 30LL * 30LL;

    {
        State state{};
        const auto d = System::decide_enemy_phase(state, false, approach_sq, now, cfg);
        assert(d.phase == Phase::ReturnHome);
    }

    {
        State state{};
        const auto d = System::decide_enemy_phase(state, true, outside_aggro_sq, now, cfg);
        assert(d.phase == Phase::ReturnHome);
    }

    {
        State state{};
        const auto d = System::decide_enemy_phase(state, true, approach_sq, now, cfg);
        assert(d.phase == Phase::Approach);
    }

    {
        State state{};
        const auto d = System::decide_enemy_phase(state, true, attack_sq, now, cfg);
        assert(d.phase == Phase::AttackWindow);
        assert(d.enter_attack_window);
        assert(d.target_should_be_committed);
    }

    {
        State state{};
        state.mode = Phase::AttackWindow;
        state.attack_window_until = now + std::chrono::milliseconds(80);
        const auto d = System::decide_enemy_phase(state, true, attack_sq, now, cfg);
        assert(d.phase == Phase::AttackWindow);
        assert(!d.leave_attack_window_to_recover);
        assert(d.target_should_be_committed);
    }

    {
        State state{};
        state.mode = Phase::AttackWindow;
        state.attack_window_until = now - std::chrono::milliseconds(1);
        const auto d = System::decide_enemy_phase(state, true, attack_sq, now, cfg);
        assert(d.phase == Phase::Recover);
        assert(d.leave_attack_window_to_recover);
        assert(!d.target_should_be_committed);
    }

    {
        State state{};
        state.mode = Phase::Recover;
        state.recover_until = now + std::chrono::milliseconds(200);
        const auto d = System::decide_enemy_phase(state, true, approach_sq, now, cfg);
        assert(d.phase == Phase::Recover);
    }

    {
        State state{};
        state.mode = Phase::AttackWindow;
        state.attack_window_until = now - std::chrono::milliseconds(1);
        Config kamikaze_cfg = cfg;
        kamikaze_cfg.kamikaze = true;
        const auto d = System::decide_enemy_phase(state, true, attack_sq, now, kamikaze_cfg);
        assert(d.phase == Phase::AttackWindow);
        assert(d.target_should_be_committed);
    }
}

