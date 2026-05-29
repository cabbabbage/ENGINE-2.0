#include "carrie_controller.hpp"

carrie_controller::carrie_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {}

void carrie_controller::on_update(const Input& in) {
    (void)in;
    Asset* player = resolve_target_player();
    (void)run_wander_behavior(player, 100, -4000, 4000);
    if (player) {
        apply_attack_hit(*player);
    }
}

void carrie_controller::on_process_pending_attacks(Asset& self) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self);
}
