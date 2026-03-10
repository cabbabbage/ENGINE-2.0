#include "bartender_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

bartender_controller::bartender_controller(Assets* assets, Asset* self)
    : assets_(assets), self_(self) {
    rng_ = std::mt19937(std::random_device{}());
    if (self_ && self_->anim_) {
        self_->anim_->set_debug_enabled(false);
        self_->needs_target = true;
    }
}

SDL_Point bartender_controller::get_random_point_in_room() {
    if (!self_) {
        return {0, 0};
    }

    std::uniform_int_distribution<int> dist(-500, 500);
    int dx = dist(rng_);
    int dy = dist(rng_);
    return {self_->world_x() + dx, self_->world_z() + dy};
}

void bartender_controller::update(const Input&) {
    if (!self_ || !self_->anim_ || !assets_) {
        return;
    }
    Asset* player = assets_->player;

    if (!player || player == self_ || player->dead || !player->active) {
        return;
    }

    int distance_sq = (self_->world_x() - player->world_x()) * (self_->world_x() - player->world_x()) + (self_->world_z() - player->world_z()) * (self_->world_z() - player->world_z());

    if (distance_sq <= 500) {
        if (self_->needs_target) {
            self_->anim_->set_animation("default");
        }
    }
    else if (self_->needs_target) {
        self_->anim_->auto_move(get_random_point_in_room());
    }

    attack_helpers::send_attack_if_hit(self_, player);
}

void bartender_controller::process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
