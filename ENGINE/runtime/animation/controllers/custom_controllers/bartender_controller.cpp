#include "bartender_controller.hpp"

bartender_controller::bartender_controller(Asset* self)
    : CustomAssetController(self),
      wander_behavior_(this, {500, -500, 500, false}) {}

void bartender_controller::on_update(const Input& in) {
    wander_behavior_.tick(in);
}

void bartender_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
