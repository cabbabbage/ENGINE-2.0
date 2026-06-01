#include "bartender_controller.hpp"

bartender_controller::bartender_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {}

void bartender_controller::on_update(const Input& in) {
    (void)in;
    Asset* player = resolve_target_player();
    (void)run_wander_behavior(player, 500, -500, 500);
    if (player) {
        (void)try_attack_target(*player, "bartender_primary", 1.0f, 72, "attack");
    }
}

void bartender_controller::on_process_pending_attacks(Asset& self) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self);
}
