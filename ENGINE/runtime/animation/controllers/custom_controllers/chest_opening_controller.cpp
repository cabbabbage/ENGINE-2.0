#include "chest_opening_controller.hpp"

#include "assets/asset/Asset.hpp"
#include "animation/attack.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"

#include <vector>

chest_opening_controller::chest_opening_controller(Asset* self)
    : CustomAssetController(self) {
}

void chest_opening_controller::on_init() {
    CustomAssetController::on_init();
}

void chest_opening_controller::on_update(const Input& in) {
    CustomAssetController::on_update(in);
    Asset* self = self_ptr();
    if (!self) {
        return;
    }

    Assets* owner_assets = assets();
    const Room* current_room = owner_assets ? owner_assets->current_room() : nullptr;
    const auto trigger_areas = owner_assets
        ? owner_assets->current_room_trigger_areas()
        : std::vector<const Room::NamedArea*>{};
    (void)current_room;
    (void)trigger_areas;
}

void chest_opening_controller::on_attack(const animation_update::Attack& attack) {
    CustomAssetController::on_attack(attack);
}

void chest_opening_controller::on_hit(const animation_update::Attack& attack) {
    CustomAssetController::on_hit(attack);
}

void chest_opening_controller::on_death() {
    CustomAssetController::on_death();
}

void chest_opening_controller::on_no_pending_attacks() {
    CustomAssetController::on_no_pending_attacks();
}

void chest_opening_controller::on_orphaned_hook(Asset& self,
                                                Asset* former_parent,
                                                std::optional<OrphanImpulse> impulse) {
    CustomAssetController::on_orphaned_hook(self, former_parent, impulse);
}

void chest_opening_controller::on_pre_delete_hook(Asset& self) {
    CustomAssetController::on_pre_delete_hook(self);
}

void chest_opening_controller::on_process_pending_attacks(Asset& self_ref) {
    CustomAssetController::on_process_pending_attacks(self_ref);
}

void chest_opening_controller::on_interact_hook(Asset& self, Asset* instigator) {
    CustomAssetController::on_interact_hook(self, instigator);
}
