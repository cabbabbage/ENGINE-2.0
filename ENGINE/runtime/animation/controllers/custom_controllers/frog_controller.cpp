#include "frog_controller.hpp"
#include "animation/controllers/custom_controllers/attack_helpers.hpp"

#include "animation/controllers/custom_controllers/controller_path_utils.hpp"
#include "animation/controllers/custom_controllers/controller_visit_threshold.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

namespace attack_helpers = animation_update::custom_controllers::attack_helpers;

frog_controller::frog_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void frog_controller::on_update(const Input&) {
    Asset* self = self_ptr();
    Assets* assets = this->assets();
    if (!self || !self->anim_ || !assets) {
        return;
    }

    Asset* player = assets->player;
    if (!player || player == self || player->dead || !player->active) {
        return;
    }

    constexpr int kFleeThresholdPx = 64;
    const int distance_sq = (self->world_x() - player->world_x()) * (self->world_x() - player->world_x()) +
                            (self->world_z() - player->world_z()) * (self->world_z() - player->world_z());

    if (distance_sq <= (kFleeThresholdPx * kFleeThresholdPx)) {
        if (!self->needs_target) {
            return;
        }
        const auto path = controller_paths::flee_path(self, player);
        if (path.empty()) {
            return;
        }
        const int visit_threshold = controller_utils::controller_visit_threshold(self, path);
        self->anim_->auto_move(path, visit_threshold);
        attack_helpers::send_attack_if_hit(self, player);
        return;
    }

    self->anim_->auto_move(player);

    attack_helpers::send_attack_if_hit(self, player);
}

void frog_controller::on_process_pending_attacks(Asset& self) {
    (void)self.process_pending_attacks();
}
