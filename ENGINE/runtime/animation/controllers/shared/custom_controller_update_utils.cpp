#include "animation/controllers/shared/custom_controller_update_utils.hpp"

#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/controller_game_context.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace animation_update::custom_controllers {

Asset* resolve_valid_player_target(const ControllerGameContext& context) {
    return context.player_is_valid() ? context.resolved_player : nullptr;
}

Asset* resolve_valid_player_target(Asset* self, Assets* assets) {
    const ControllerGameContext context = build_controller_game_context(self, assets);
    return resolve_valid_player_target(context);
}

void dispatch_contact_attack(const ControllerGameContext& context) {
    Asset* self = context.self;
    Asset* player = resolve_valid_player_target(context);
    if (!self || !player) {
        return;
    }
    AttackDetectionHelper::send_attack_if_hit(self, player);
}

void dispatch_contact_attack(Asset* self, Asset* player) {
    if (!self || !player || self == player || player->dead || !player->active) {
        return;
    }

    AttackDetectionHelper::send_attack_if_hit(self, player);
}

} // namespace animation_update::custom_controllers
