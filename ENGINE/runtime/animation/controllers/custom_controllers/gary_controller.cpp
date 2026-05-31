#include "gary_controller.hpp"

gary_controller::gary_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {}

void gary_controller::on_update(const Input& in) {
    (void)in;
    Asset* player = resolve_target_player();
    (void)run_wander_behavior(player, 500, -500, 500);
    if (player) {
        apply_attack_hit(*player);
    }
}

void gary_controller::on_process_pending_attacks(Asset& self) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self);
}
