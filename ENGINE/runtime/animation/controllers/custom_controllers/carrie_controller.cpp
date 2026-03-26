#include "carrie_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/range_util.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

carrie_controller::carrie_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    rng_ = std::mt19937(std::random_device{}());
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(true);
        owner->needs_target = true;
    }
}

SDL_Point carrie_controller::get_random_point_in_room() {
    Asset* self = self_ptr();
    if (!self) {
        return {0, 0};
    }

    std::uniform_int_distribution<int> dist(-4000, 4000);
    int dx = dist(rng_);
    int dz = dist(rng_);
    return {self->world_x() + dx, self->world_z() + dz};
}

void carrie_controller::on_update(const Input&) {
    constexpr int kIdleRangePx = 100;

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
    } else if (self->needs_target) {
        self->anim_->auto_move(get_random_point_in_room());
    }

    attack_helpers::send_attack_if_hit(self, player);
}

void carrie_controller::on_process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
