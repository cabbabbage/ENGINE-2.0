#include "fly_controller.hpp"
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/animation_update.hpp"
#include "animation/attack.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"
#include "utils/input.hpp"

namespace {

custom_controller_api::RandomOrbit3DControllerBehaviorConfig
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
    Asset* self = self_ptr();
    if (!self) {
        return;
    }

    orbit_behavior_.set_config(game_context().fly_orbit_behavior_config());
    if(orbiting) {
        orbit_behavior_.tick(in);
        if (game_context().room_flies_aggressive()) {
            animation_update::custom_controllers::AttackDetectionHelper::send_attacks_to_active_targets(self, assets());
        }
    }
    else{
        if (self->anim_) {
            self->anim_->set_animation(animation_update::detail::kDefaultAnimation);
        }
    }
}

void fly_controller::on_process_pending_attacks(Asset& self_ref) {
    CustomAssetController::on_process_pending_attacks(self_ref);
}

void fly_controller::on_attack(const animation_update::Attack& attack) {
    Asset* self = self_ptr();
    if (!self) {
        return;
    }
    if (attack.attacker_asset_name == "vibble_attack_1" || attack.attacker_asset_name == "vibble") {
        if (Assets* owner_assets = self->get_assets()) {
            std::string room_name = self->owning_room_name();
            if (room_name.empty()) {
                if (Room* room = owner_assets->current_room()) {
                    room_name = room->room_name;
                }
            }
            owner_assets->mutable_game_context().set_room_fly_aggression(room_name, 20.0f);
        }

        if (self->anim_) {
            self->anim_->cancel_all_movement();
            self->anim_->set_animation(animation_update::detail::kDefaultAnimation);
        }
        self->move_to_world_position(self->world_x(), -10, self->world_z(), self->grid_resolution);
        orbiting = false;
    }
}
