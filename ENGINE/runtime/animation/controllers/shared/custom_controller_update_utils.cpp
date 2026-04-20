#include "animation/controllers/shared/custom_controller_update_utils.hpp"

#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/animation_update.hpp"
#include "animation/controllers/shared/controller_game_context.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace animation_update::custom_controllers {

Asset* resolve_valid_player_target(const ControllerGameContext& context) {
    return context.player_is_valid() ? context.resolved_player : nullptr;
}

Asset* resolve_valid_player_target(Asset* self, Assets* assets) {
    if (!self || !assets) {
        return nullptr;
    }

    const runtime::context::GameRuntimeContext& shared = assets->game_context();
    Asset* player = shared.player();
    if (!player) {
        player = assets->player;
    }
    if (!player || player == self || player->dead || !player->active) {
        return nullptr;
    }
    return player;
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

void begin_reverse_current_animation_until_stop(const ControllerGameContext& context) {
    begin_reverse_current_animation_until_stop(context.self);
}

void begin_reverse_current_animation_until_stop(Asset* self) {
    if (!self || !self->anim_) {
        return;
    }
    self->anim_->begin_reverse_current_animation_until_stop();
}

void begin_reverse_current_animation_to_default(const ControllerGameContext& context) {
    begin_reverse_current_animation_to_default(context.self);
}

void begin_reverse_current_animation_to_default(Asset* self) {
    if (!self || !self->anim_) {
        return;
    }
    self->anim_->begin_reverse_current_animation_to_default();
}

void stop_reverse_current_animation(const ControllerGameContext& context) {
    stop_reverse_current_animation(context.self);
}

void stop_reverse_current_animation(Asset* self) {
    if (!self || !self->anim_) {
        return;
    }
    self->anim_->stop_reverse_current_animation();
}

} // namespace animation_update::custom_controllers
