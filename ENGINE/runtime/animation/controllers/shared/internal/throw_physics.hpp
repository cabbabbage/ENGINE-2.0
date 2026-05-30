#pragma once

#include <algorithm>
#include <cmath>

namespace animation_update::custom_controllers::internal::throw_physics {

struct Tuning {
    double gravity_units_per_sec2 = 2400.0;
    double max_dt_seconds = 1.0 / 20.0;
    double min_bounce_speed = 60.0;
    double horizontal_damping_per_60hz = 0.90;
    double horizontal_bounce_friction = 0.65;
    double settle_vertical_speed = 20.0;
    double settle_horizontal_speed = 18.0;
};

struct StepState {
    double world_x = 0.0;
    double world_z = 0.0;
    double world_y = 0.0;
    double floor_y = 0.0;
    double velocity_x = 0.0;
    double velocity_z = 0.0;
    double velocity_y = 0.0;
    double restitution = 0.0;
    bool active = false;
};

struct StepResult {
    bool hit_floor = false;
    double impact_speed = 0.0;
    double horizontal_speed = 0.0;
};

inline Tuning default_tuning() {
    return Tuning{};
}

inline StepResult step(StepState& state, double dt_seconds, const Tuning& tuning = default_tuning()) {
    StepResult out{};
    const double dt = std::clamp(dt_seconds, 1.0 / 240.0, tuning.max_dt_seconds);
    state.velocity_y -= tuning.gravity_units_per_sec2 * dt;
    state.world_x += state.velocity_x * dt;
    state.world_z += state.velocity_z * dt;
    state.world_y += state.velocity_y * dt;

    const double horizontal_damping = std::pow(tuning.horizontal_damping_per_60hz, dt * 60.0);
    state.velocity_x *= horizontal_damping;
    state.velocity_z *= horizontal_damping;

    if (state.world_y <= state.floor_y) {
        state.world_y = state.floor_y;
        out.hit_floor = true;
        out.impact_speed = std::abs(state.velocity_y);
        const double rebound_speed = out.impact_speed * state.restitution;
        if (rebound_speed > tuning.min_bounce_speed) {
            state.velocity_y = rebound_speed;
            state.velocity_x *= tuning.horizontal_bounce_friction;
            state.velocity_z *= tuning.horizontal_bounce_friction;
        } else {
            state.velocity_y = 0.0;
        }
    }

    out.horizontal_speed = std::hypot(state.velocity_x, state.velocity_z);
    state.active =
        state.world_y > state.floor_y + 0.5 ||
        std::abs(state.velocity_y) > tuning.settle_vertical_speed ||
        out.horizontal_speed > tuning.settle_horizontal_speed;

    if (!state.active &&
        std::abs(state.world_y - state.floor_y) <= 0.5 &&
        std::abs(state.velocity_y) <= tuning.settle_vertical_speed &&
        out.horizontal_speed <= tuning.settle_horizontal_speed) {
        state.world_y = state.floor_y;
        state.velocity_x = 0.0;
        state.velocity_z = 0.0;
        state.velocity_y = 0.0;
        out.horizontal_speed = 0.0;
    }
    return out;
}

inline int damage_from_impact_energy(double mass_kg, double speed_units_per_sec) {
    if (!std::isfinite(mass_kg) || !std::isfinite(speed_units_per_sec) || mass_kg <= 0.0 || speed_units_per_sec <= 0.0) {
        return 0;
    }
    // Runtime units are pixel-ish, so tune with a scale factor for gameplay-sized values.
    constexpr double kEnergyToDamage = 1.0 / 60000.0;
    constexpr int kMaxThrowDamage = 500;
    const double energy = 0.5 * mass_kg * speed_units_per_sec * speed_units_per_sec;
    const int damage = static_cast<int>(std::lround(energy * kEnergyToDamage));
    return std::clamp(damage, 0, kMaxThrowDamage);
}

} // namespace animation_update::custom_controllers::internal::throw_physics
