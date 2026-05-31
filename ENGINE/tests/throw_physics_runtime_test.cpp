#include "animation/controllers/shared/internal/throw_physics.hpp"

#include <cassert>
#include <cmath>

int main() {
    using namespace animation_update::custom_controllers::internal::throw_physics;

    {
        StepState state{};
        state.world_y = 100.0;
        state.floor_y = 0.0;
        state.velocity_y = 0.0;
        state.active = true;
        const auto r = step(state, 1.0 / 60.0);
        assert(!r.hit_floor);
        assert(state.world_y < 100.0);
    }

    {
        StepState state{};
        state.world_y = 1.0;
        state.floor_y = 0.0;
        state.velocity_y = -600.0;
        state.restitution = 0.0;
        state.active = true;
        const auto r = step(state, 1.0 / 60.0);
        assert(r.hit_floor);
        assert(state.world_y == 0.0);
    }

    {
        const int d0 = damage_from_impact_energy(10.0, 0.0);
        const int d1 = damage_from_impact_energy(10.0, 100.0);
        const int d2 = damage_from_impact_energy(10.0, 200.0);
        assert(d0 == 0);
        assert(d2 >= d1);
    }

    return 0;
}
