#include "animation/controllers/shared/wander_controller_behavior.hpp"

#include <algorithm>

#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/custom_asset_controller.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/range_util.hpp"

namespace animation_update::custom_controllers {

WanderControllerBehavior::WanderControllerBehavior(CustomAssetController* controller,
                                                   WanderControllerBehaviorConfig config)
    : controller_(controller),
      config_(config),
      rng_(std::random_device{}()),
      random_range_() {
    const int lower = std::min(config_.random_range_min, config_.random_range_max);
    const int upper = std::max(config_.random_range_min, config_.random_range_max);
    random_range_ = std::uniform_int_distribution<int>(lower, upper);

    if (!controller_) {
        return;
    }

    Asset* self = controller_->self_ptr();
    if (self && self->anim_) {
        self->anim_->set_debug_enabled(config_.debug_enabled);
        self->needs_target = true;
    }
}

void WanderControllerBehavior::tick([[maybe_unused]] const Input& in) {
    if (!controller_) {
        return;
    }

    Asset* self = controller_->self_ptr();
    Assets* assets = controller_->assets();
    if (!self || !self->anim_ || !assets) {
        return;
    }

    Asset* player = assets->player;
    if (!player || player == self || player->dead || !player->active) {
        return;
    }

    if (Range::is_in_range(self, player, config_.idle_radius_px)) {
        if (!self->needs_target) {
            self->anim_->cancel_all_movement();
        }
        if (self->needs_target) {
            self->anim_->set_animation("default");
        }
    } else if (self->needs_target) {
        self->anim_->auto_move(get_random_target());
    }

    AttackDetectionHelper::send_attack_if_hit(self, player);
}

SDL_Point WanderControllerBehavior::get_random_target() const {
    if (!controller_) {
        return {0, 0};
    }

    Asset* self = controller_->self_ptr();
    if (!self) {
        return {0, 0};
    }

    const int dx = random_range_(rng_);
    const int dz = random_range_(rng_);
    return {self->world_x() + dx, self->world_z() + dz};
}

} // namespace animation_update::custom_controllers
