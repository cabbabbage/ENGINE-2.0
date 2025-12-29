#include "Carrie_controller.hpp"
#include "asset/Asset.hpp"
#include "core/AssetsManager.hpp"

CarrieController::CarrieController(Assets* assets, Asset* self)
    : assets_(assets), self_(self) {
    if (self_ && self_->anim_) {
        self_->anim_->set_debug_enabled(false);
        self_->needs_target = true;
    }
}

SDL_Point CarrieController::get_random_point_in_room() {
    if (!assets_ || !self_) {
        return {0, 0};
    }

    const std::string& room_name = self_->owning_room_name();
    for (Room* room : assets_->rooms()) {
        if (room && room->room_name == room_name && room->room_area) {
            return room->room_area->random_point_within();
        }
    }

    return {0, 0};
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

    if (distance_sq <= 500) {
            self_->anim_->set_animation("default");
       
    }
    else if (self_->needs_target) {
        self_->anim_->auto_move(get_random_point_in_room());
    }
}
