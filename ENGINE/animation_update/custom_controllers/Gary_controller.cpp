#include "Gary_controller.hpp"
#include "asset/Asset.hpp"
#include "core/AssetsManager.hpp"

GaryController::GaryController(Assets* assets, Asset* self)
    : assets_(assets), self_(self) {
    rng_ = std::mt19937(std::random_device{}());
    if (self_ && self_->anim_) {
        self_->anim_->set_debug_enabled(false);
        self_->needs_target = true;
    }
}

SDL_Point GaryController::get_random_point_in_room() {
    if (!self_) {
        return {0, 0};
    }

    std::uniform_int_distribution<int> dist(-100, 100);
    int dx = dist(rng_);
    int dy = dist(rng_);
    return {self_->pos.x + dx, self_->pos.y + dy};
}

void GaryController::update(const Input&) {
    if (!self_ || !self_->anim_ || !assets_) {
        return;
    }
    Asset* player = assets_->player;

    if (!player || player == self_ || player->dead || !player->active) {
        return;
    }

    int distance_sq = (self_->pos.x - player->pos.x) * (self_->pos.x - player->pos.x) + (self_->pos.y - player->pos.y) * (self_->pos.y - player->pos.y);

    if (distance_sq <= 500) {
            self_->anim_->set_animation("default");
       
    }
    else if (self_->needs_target) {
        self_->anim_->auto_move(get_random_point_in_room());
    }
}
