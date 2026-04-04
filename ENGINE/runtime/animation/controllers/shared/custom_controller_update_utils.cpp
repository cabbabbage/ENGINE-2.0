#include "animation/controllers/shared/custom_controller_update_utils.hpp"

#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace animation_update::custom_controllers {

namespace {

bool is_valid_player_target(Asset* self, Asset* player) {
    if (!player || !self || player == self) {
        return false;
    }

    if (player->dead || !player->active) {
        return false;
    }

    return true;
}

} // namespace

Asset* resolve_valid_player_target(Asset* self, Assets* assets) {
    if (!self || !assets) {
        return nullptr;
    }

    Asset* player = assets->player;
    return is_valid_player_target(self, player) ? player : nullptr;
}

void dispatch_contact_attack(Asset* self, Asset* player) {
    if (!is_valid_player_target(self, player)) {
        return;
    }

    AttackDetectionHelper::send_attack_if_hit(self, player);
}

} // namespace animation_update::custom_controllers
