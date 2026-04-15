#include "bomb_controller.hpp"
#include "animation/controllers/shared/custom_controller_update_utils.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/range_util.hpp"
#include <iostream>
#include <optional>
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
namespace animation_update::custom_controllers {}
namespace custom_controllers = animation_update::custom_controllers;

bomb_controller::bomb_controller(Asset* self)
    : CustomAssetController(self) {
        std::cout<<"bomb Controller Connected";
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void bomb_controller::on_update(const Input&) {
    constexpr int kbombStopRadiusPx = 60;

    const auto& ctx = game_context();
    Asset* self = self_ptr();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }
    Asset* player = animation_update::custom_controllers::resolve_valid_player_target(ctx);

    if (!player) {
        return;
    }

    const bool in_attack_range = Range::is_in_range(self, player, kbombStopRadiusPx);
    if (in_attack_range) {
        if (!self->needs_target) {
            self->anim_->cancel_all_movement();
        }
    } else if (self->needs_target) {
        self->anim_->auto_move_3d({player->world_x(), player->world_y(), player->world_z()}, 0, std::nullopt, false);
    }

    animation_update::custom_controllers::dispatch_contact_attack(ctx);
}

void bomb_controller::on_process_pending_attacks(Asset& self) {
    custom_controllers::AttackProcessingHelper::process_pending_attacks(self);
}
