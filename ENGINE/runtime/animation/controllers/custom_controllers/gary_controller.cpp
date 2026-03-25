#include "gary_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

gary_controller::gary_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    rng_ = std::mt19937(std::random_device{}());
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

SDL_Point gary_controller::get_random_point_in_room() {
    Asset* self = self_ptr();
    if (!self) {
        return {0, 0};
    }

    std::uniform_int_distribution<int> dist(-500, 500);
    int dx = dist(rng_);
    int dy = dist(rng_);
    return {self->world_x() + dx, self->world_z() + dy};
}

void gary_controller::on_update(const Input&) {
    Asset* self = self_ptr();
    Assets* assets = this->assets();
    if (!self || !self->anim_ || !assets) {
        return;
    }
    Asset* player = assets->player;

    if (!player || player == self || player->dead || !player->active) {
        return;
    }

    int distance_sq = (self->world_x() - player->world_x()) * (self->world_x() - player->world_x()) + (self->world_z() - player->world_z()) * (self->world_z() - player->world_z());

    if (distance_sq <= 500) {
        if (self->needs_target) {
            self->anim_->set_animation("default");
        }
    } else if (self->needs_target) {
        self->anim_->auto_move(get_random_point_in_room());
    }

    attack_helpers::send_attack_if_hit(self, player);
}

void gary_controller::on_process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
