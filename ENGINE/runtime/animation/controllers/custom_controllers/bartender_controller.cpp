#include "bartender_controller.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/range_util.hpp"

bartender_controller::bartender_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    rng_ = std::mt19937(std::random_device{}());
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

SDL_Point bartender_controller::get_random_point_in_room() {
    Asset* self = self_ptr();
    if (!self) {
        return {0, 0};
    }

    std::uniform_int_distribution<int> dist(-500, 500);
    int dx = dist(rng_);
    int dz = dist(rng_);
    return {self->world_x() + dx, self->world_z() + dz};
}

void bartender_controller::on_update(const Input&) {
    constexpr int kIdleRangePx = 500;

    Asset* self = self_ptr();
    Assets* assets = this->assets();
    if (!self || !self->anim_ || !assets) {
        return;
    }
    Asset* player = assets->player;

    if (!player || player == self || player->dead || !player->active) {
        return;
    }

    const bool in_idle_range = Range::is_in_range(self, player, kIdleRangePx);
    if (in_idle_range) {
        if (!self->needs_target) {
            self->anim_->cancel_all_movement();
        }
        if (self->needs_target) {
            self->anim_->set_animation("default");
        }
    }
    else if (self->needs_target) {
        self->anim_->auto_move(get_random_point_in_room());
    }

    animation_update::custom_controllers::AttackDetectionHelper::send_attack_if_hit(self, player);
}

void bartender_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
