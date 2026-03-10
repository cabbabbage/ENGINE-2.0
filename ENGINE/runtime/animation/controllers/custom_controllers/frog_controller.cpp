#include "frog_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"

#include "animation/controllers/custom_controllers/controller_path_utils.hpp"
#include "animation/controllers/custom_controllers/controller_visit_threshold.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

frog_controller::frog_controller(Assets* assets, Asset* self)
    : assets_(assets), self_(self) {
    if (self_ && self_->anim_) {
        self_->anim_->set_debug_enabled(false);
        self_->needs_target = true;
    }
}

void frog_controller::update(const Input&) {
    if (!self_ || !self_->anim_ || !assets_) {
        return;
    }

    Asset* player = assets_->player;
    if (!player || player == self_ || player->dead || !player->active) {
        return;
    }

    constexpr int kFleeThresholdPx = 64;
    const int distance_sq = (self_->world_x() - player->world_x()) * (self_->world_x() - player->world_x()) +
                            (self_->world_z() - player->world_z()) * (self_->world_z() - player->world_z());

    if (distance_sq <= (kFleeThresholdPx * kFleeThresholdPx)) {
        if (!self_->needs_target) {
            return;
        }
        const auto path = controller_paths::flee_path(self_, player);
        if (path.empty()) {
            return;
        }
        const int visit_threshold = controller_utils::controller_visit_threshold(self_, path);
        self_->anim_->auto_move(path, visit_threshold);
        attack_helpers::send_attack_if_hit(self_, player);
        return;
    }

    self_->anim_->auto_move(player);

    attack_helpers::send_attack_if_hit(self_, player);
}

void frog_controller::process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
