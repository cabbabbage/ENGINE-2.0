#include "gary_controller.hpp"

gary_controller::gary_controller(Asset* self)
    : CustomAssetController(self),
      wander_behavior_(this, {500, -500, 500, false}) {}

void gary_controller::on_update(const Input& in) {
    wander_behavior_.tick(in);
}

void gary_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
