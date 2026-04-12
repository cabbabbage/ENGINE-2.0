// CONTROLLER_META_BEGIN
// Controller: fly_controller
// Asset: fly (type: object)
// Available animations [1]:
//   - default
// Generated: 2026-04-10 02:16:39
// CONTROLLER_META_END


#include <iostream>
#include "fly_controller.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/input.hpp"

fly_controller::fly_controller(Asset* self)
    : CustomAssetController(self),
      orbit_behavior_(
          this,
          [] {
              animation_update::custom_controllers::RandomOrbit3DControllerBehaviorConfig cfg{};
              cfg.visit_threshold_px = 18;
              cfg.orbit_radius_px = 10;
              cfg.orbit_vertical_amplitude_px = 36;
              cfg.orbit_segment_checkpoints = 4;
              cfg.orbit_enter_distance_px = 280;
              cfg.orbit_exit_distance_px = 420;
              cfg.approach_checkpoint_count = 5;
              cfg.approach_min_wave_px = 18;
              cfg.approach_max_wave_px = 160;
              cfg.approach_vertical_wave_px = 48;
              cfg.orbit_angular_velocity_radians = 0.45;
              cfg.retarget_blend_step = 0.35;
              cfg.debug_enabled = false;
              cfg.override_non_locked = true;
              return cfg;
          }()) {
    Asset* owner = self_ptr();
    if (owner) {
        // Ensure the orbit behavior can issue its first movement plan immediately.
        owner->needs_target = true;
    }
}

void fly_controller::on_update(const Input& in) {
    if(orbiting) {
        
        orbit_behavior_.tick(in, !orbiting);
    }
    else{
        if (self_ptr()->needs_target) {
            self_ptr()->anim_->set_animation("die");

        }
    }
}

void fly_controller::on_process_pending_attacks(Asset& self_ref) {
    (void)self_ref;

    const auto pending_attacks = self_ref.process_pending_attacks();
    if(pending_attacks.empty()) {
        return;
    }
    std::cout<<"fly_controller::on_process_pending_attacks called, transitioning to non-orbiting state."<<std::endl;
    orbiting = false;

    self_ref.anim_->cancel_all_movement();
    self_ref.anim_->auto_move_3d({self_ref.world_x(), -10, self_ref.world_z()}, 0, std::nullopt, true);



}
