#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "core/axis_convention.hpp"

class Input;
class Asset;
class CustomAssetController;

namespace animation_update::custom_controllers {

struct RandomOrbit3DControllerBehaviorConfig {
    int visit_threshold_px = 48;
    int orbit_radius_px = 180;
    int orbit_vertical_amplitude_px = 36;
    int orbit_segment_checkpoints = 4;
    int orbit_enter_distance_px = 280;
    int orbit_exit_distance_px = 420;
    int approach_checkpoint_count = 5;
    int approach_min_wave_px = 18;
    int approach_max_wave_px = 160;
    int approach_vertical_wave_px = 48;
    double orbit_angular_velocity_radians = 0.45;
    double retarget_blend_step = 0.35;
    bool debug_enabled = false;
    bool override_non_locked = true;
};

class RandomOrbit3DControllerBehavior {
public:
    enum class Phase {
        Approach,
        Orbit,
        RetargetTransition
    };

    RandomOrbit3DControllerBehavior(CustomAssetController* controller,
                                    RandomOrbit3DControllerBehaviorConfig config = {});

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
