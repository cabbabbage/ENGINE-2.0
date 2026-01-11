#include "Carrie_controller.hpp"
#include "animation_update/custom_controllers/attack_helpers.hpp"
#include "asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

CarrieController::CarrieController(Assets* assets, Asset* self)
    : assets_(assets), self_(self) {
    rng_ = std::mt19937(std::random_device{}());
    if (self_ && self_->anim_) {
        self_->anim_->set_debug_enabled(true);
        self_->needs_target = true;
    }
}

SDL_Point CarrieController::get_random_point_in_room() {
    if (!self_) {
        return {0, 0};
    }

    std::uniform_int_distribution<int> dist(-4000, 4000);
    int dx = dist(rng_);
    int dy = dist(rng_);
    return {self_->pos.x + dx, self_->pos.y + dy};
}

void CarrieController::update(const Input&) {
    if (!self_ || !self_->anim_ || !assets_) {
        return;
    }
    Asset* player = assets_->player;

    if (!player || player == self_ || player->dead || !player->active) {
        return;
    }

    int distance_sq = (self_->pos.x - player->pos.x) * (self_->pos.x - player->pos.x) + (self_->pos.y - player->pos.y) * (self_->pos.y - player->pos.y);

    if (distance_sq <= 100) {
        if (self_->needs_target) {
            self_->anim_->set_animation("default");
        }
    }
    else if (self_->needs_target) {
        self_->anim_->auto_move(get_random_point_in_room());
    }

    attack_helpers::send_attack_if_hit(self_, player);
}

void CarrieController::process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
