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
#include "core/AssetsManager.hpp"
#include "utils/input.hpp"

namespace {

animation_update::custom_controllers::RandomOrbit3DControllerBehaviorConfig
resolve_fly_orbit_behavior_config(Asset* self) {
    if (!self) {
        return runtime::config::make_default_fly_orbit_behavior_config();
    }
    Assets* owner_assets = self->get_assets();
    if (!owner_assets) {
        return runtime::config::make_default_fly_orbit_behavior_config();
    }
    return owner_assets->runtime_game_config().fly_orbit_behavior;
}

} // namespace

fly_controller::fly_controller(Asset* self)
    : CustomAssetController(self),
      orbit_behavior_(this, resolve_fly_orbit_behavior_config(self)) {
    Asset* owner = self_ptr();
    if (owner) {
        // Ensure the orbit behavior can issue its first movement plan immediately.
        owner->needs_target = true;
    }
}

void fly_controller::on_update(const Input& in) {
    orbit_behavior_.set_config(game_context().fly_orbit_behavior_config());
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

    for (const auto& attack : pending_attacks) {
        if (attack.attacker_asset_name == "vibble_attack_1" || attack.attacker_asset_name == "vibble") {

        
            
            std::cout<<"fly_controller::on_process_pending_attacks called, transitioning to non-orbiting state."<<std::endl;


            self_ref.anim_->cancel_all_movement();
            self_ref.anim_->auto_move_3d({self_ref.world_x(), -10, self_ref.world_z()}, 0, std::nullopt, true);
            orbiting = false;
            return;    
    
        }


    }
}