#include "animation/controllers/shared/default_custom_controller.hpp"

namespace animation_update::custom_controllers {

DefaultCustomController::DefaultCustomController(Asset* self, bool generic_fallback)
    : ::CustomAssetController(self, generic_fallback) {}

DefaultCustomController::~DefaultCustomController() = default;

void DefaultCustomController::on_init() {
    ::CustomAssetController::on_init();
}

void DefaultCustomController::on_update(const Input& in) {
    ::CustomAssetController::on_update(in);
}

void DefaultCustomController::on_attack(const animation_update::Attack& attack) {
    ::CustomAssetController::on_attack(attack);
}

void DefaultCustomController::on_hit(const animation_update::Attack& attack) {
    ::CustomAssetController::on_hit(attack);
}

void DefaultCustomController::on_death() {
    ::CustomAssetController::on_death();
}

void DefaultCustomController::on_no_pending_attacks() {
    ::CustomAssetController::on_no_pending_attacks();
}

void DefaultCustomController::on_after_attack() {
    ::CustomAssetController::on_after_attack();
}

AttackProcessingConfig DefaultCustomController::attack_processing_config() const {
    return ::CustomAssetController::attack_processing_config();
}

void DefaultCustomController::on_process_pending_attacks(Asset& self) {
    ::CustomAssetController::on_process_pending_attacks(self);
}

void DefaultCustomController::on_pre_delete_hook(Asset& self) {
    ::CustomAssetController::on_pre_delete_hook(self);
}

void DefaultCustomController::on_orphaned_hook(Asset& self,
                                               Asset* former_parent,
                                               std::optional<OrphanImpulse> impulse) {
    ::CustomAssetController::on_orphaned_hook(self, former_parent, impulse);
}

void DefaultCustomController::on_interact_hook(Asset& self, Asset* instigator) {
    ::CustomAssetController::on_interact_hook(self, instigator);
}

} // namespace animation_update::custom_controllers
