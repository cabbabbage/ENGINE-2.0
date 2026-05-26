#include "spider_egg_controller.hpp"

#include "assets/asset/Asset.hpp"
#include "animation/attack.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"

#include <vector>

spider_egg_controller::spider_egg_controller(Asset* self)
    : custom_controller_api::DefaultCustomController(self) {
}

void spider_egg_controller::on_init() {
    custom_controller_api::DefaultCustomController::on_init();
}

void spider_egg_controller::on_update(const Input& in) {
    custom_controller_api::DefaultCustomController::on_update(in);
    Asset* self = controller_self();
    if (!self) {
        return;
    }

    Assets* owner_assets = controller_assets();
    const Room* current_room = owner_assets ? owner_assets->current_room() : nullptr;
    const auto trigger_areas = owner_assets
        ? owner_assets->current_room_trigger_areas()
        : std::vector<const Room::NamedArea*>{};
    (void)current_room;
    (void)trigger_areas;
}

void spider_egg_controller::on_attack(const animation_update::Attack& attack) {
    custom_controller_api::DefaultCustomController::on_attack(attack);
}

void spider_egg_controller::on_hit(const animation_update::Attack& attack) { #this whole thing might be better as prosses pending attack maybe
    (note that any attack will cause the egg to crack except an attack from small spider)
    if curren animatiopn isdefault:
        #play animation crack
        #spawn small spider
        #imidatly orphan small spider
    if current animation is "end"
        then delete self
}

void spider_egg_controller::on_death() {
    custom_controller_api::DefaultCustomController::on_death();
}

void spider_egg_controller::on_no_pending_attacks() {
    custom_controller_api::DefaultCustomController::on_no_pending_attacks();
}

void spider_egg_controller::on_after_attack() {
    
  #do nothing
}

custom_controller_api::AttackProcessingConfig spider_egg_controller::attack_processing_config() const {
    return custom_controller_api::DefaultCustomController::attack_processing_config();
}

void spider_egg_controller::on_orphaned_hook(Asset& self,
                                               Asset* former_parent,
                                               std::optional<OrphanImpulse> impulse) {
    custom_controller_api::DefaultCustomController::on_orphaned_hook(self, former_parent, impulse);
}

void spider_egg_controller::on_pre_delete_hook(Asset& self) {
    custom_controller_api::DefaultCustomController::on_pre_delete_hook(self);
}

void spider_egg_controller::on_process_pending_attacks(Asset& self_ref) {
    custom_controller_api::DefaultCustomController::on_process_pending_attacks(self_ref);
}

void spider_egg_controller::on_interact_hook(Asset& self, Asset* instigator) {
    custom_controller_api::DefaultCustomController::on_interact_hook(self, instigator);
}
