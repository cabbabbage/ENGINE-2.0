#pragma once

namespace runtime::config {

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

inline RandomOrbit3DControllerBehaviorConfig make_default_fly_orbit_behavior_config() {
    RandomOrbit3DControllerBehaviorConfig cfg{};
    cfg.visit_threshold_px = 18;
    cfg.orbit_radius_px = 10;
    cfg.orbit_vertical_amplitude_px = 306;
    cfg.orbit_segment_checkpoints = 4;
    cfg.orbit_enter_distance_px = 80;
    cfg.orbit_exit_distance_px = 120;
    cfg.approach_checkpoint_count = 5;
    cfg.approach_min_wave_px = 18;
    cfg.approach_max_wave_px = 30;
    cfg.approach_vertical_wave_px = 48;
    cfg.orbit_angular_velocity_radians = 0.45;
    cfg.retarget_blend_step = 0.35;
    cfg.debug_enabled = false;
    cfg.override_non_locked = true;
    return cfg;
}

struct RuntimeGameConfig {
    RandomOrbit3DControllerBehaviorConfig fly_orbit_behavior =
        make_default_fly_orbit_behavior_config();
};

} // namespace runtime::config
