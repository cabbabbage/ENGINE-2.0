#include <cassert>

#include "animation/controllers/shared/internal/controller_combat_system.hpp"

int main() {
    using Combat = animation_update::custom_controllers::internal::ControllerCombatSystem;

    // Cooldown must start only when an attack dispatch actually hit.
    assert(Combat::should_start_cooldown_after_attack(true));
    assert(!Combat::should_start_cooldown_after_attack(false));

    // Active-target dispatch result must be true only when at least one hit was dispatched.
    assert(!Combat::has_any_dispatched_hits(0));
    assert(Combat::has_any_dispatched_hits(1));
    assert(Combat::has_any_dispatched_hits(7));
}

