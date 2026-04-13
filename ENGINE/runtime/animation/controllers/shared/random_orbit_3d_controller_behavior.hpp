#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "core/axis_convention.hpp"
#include "core/runtime_game_config.hpp"

class Input;
class Asset;
class CustomAssetController;

namespace animation_update::custom_controllers {

using RandomOrbit3DControllerBehaviorConfig =
    runtime::config::RandomOrbit3DControllerBehaviorConfig;

class RandomOrbit3DControllerBehavior {
public:
    enum class Phase {
        Approach,
        Orbit,
        RetargetTransition
    };

    RandomOrbit3DControllerBehavior(CustomAssetController* controller,
                                    RandomOrbit3DControllerBehaviorConfig config = {});
    void set_config(RandomOrbit3DControllerBehaviorConfig config);

    void tick(const Input& in, bool mad);

private:
    bool ingest_target_snapshot(const axis::WorldPos& target,
                                std::uint32_t target_version,
                                int resolution_layer,
                                bool changed_hint,
                                const axis::WorldPos& self_pos);
    void reset_for_missing_target(Asset& self);
    void apply_default_idle(Asset& self) const;

    int visit_threshold_px(const Asset& self) const;
    std::optional<int> checkpoint_resolution_override() const;

    bool should_enter_orbit(const axis::WorldPos& self_pos) const;
    bool should_exit_orbit(const axis::WorldPos& self_pos) const;

    axis::WorldPos blended_retarget_target() const;
    void calibrate_orbit_phase(const axis::WorldPos& self_pos, const axis::WorldPos& center);

    std::vector<axis::WorldPos> build_approach_checkpoints(const axis::WorldPos& self_pos,
                                                            const axis::WorldPos& target,
                                                            bool soft_transition) const;
    std::vector<axis::WorldPos> build_orbit_checkpoints(const axis::WorldPos& self_pos) const;
    void set_state_mad();
private:
    CustomAssetController* controller_ = nullptr;
    RandomOrbit3DControllerBehaviorConfig config_{};

    std::mt19937 rng_{};
    std::uniform_real_distribution<double> orbit_radius_jitter_{0.92, 1.08};

    Phase phase_ = Phase::Approach;
    bool has_target_ = false;
    axis::WorldPos target_{0, 0, 0};
    axis::WorldPos previous_target_{0, 0, 0};
    std::uint32_t target_version_ = 0;
    int checkpoint_resolution_layer_ = 0;

    double orbit_angle_ = 0.0;
    double orbit_radius_current_ = 0.0;
    double orbit_radius_goal_ = 0.0;
    double angular_velocity_ = 0.0;
    double approach_wave_phase_ = 0.0;
    double retarget_alpha_ = 1.0;
};

} // namespace animation_update::custom_controllers
