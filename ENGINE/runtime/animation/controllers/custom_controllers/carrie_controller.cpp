#include "carrie_controller.hpp"

carrie_controller::carrie_controller(Asset* self)
    : CustomAssetController(self),
      wander_behavior_(this, {100, -4000, 4000, true}) {}

void carrie_controller::on_update(const Input& in) {
    wander_behavior_.tick(in);
}

void carrie_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
