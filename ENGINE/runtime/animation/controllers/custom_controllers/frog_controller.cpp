#include "frog_controller.hpp"
#include "animation/controllers/shared/custom_controller_update_utils.hpp"
#include "animation/animation_update.hpp"

#include "animation/controllers/shared/controller_path_utils.hpp"
#include "animation/controllers/shared/controller_visit_threshold.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/range_util.hpp"

frog_controller::frog_controller(Asset* self)
    : CustomAssetController(self) {
    Asset* owner = self_ptr();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
    }
}

void frog_controller::on_update(const Input&) {
    const auto& ctx = game_context();
    Asset* self = self_ptr();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = animation_update::custom_controllers::resolve_valid_player_target(ctx);
    if (!player) {
        return;
    }

    constexpr int kFleeThresholdPx = 64;
    const bool in_flee_range = Range::is_in_range(self, player, kFleeThresholdPx);
    if (in_flee_range) {
        if (!self->needs_target) {
            return;
        }
        const auto path = controller_paths::flee_path(self, player);
        if (path.empty()) {
            return;
        }
        const int visit_threshold = controller_utils::controller_visit_threshold(self, path);
        self->anim_->auto_move(path, visit_threshold);
        animation_update::custom_controllers::dispatch_contact_attack(ctx);
        return;
    }

    self->anim_->auto_move(player);

    animation_update::custom_controllers::dispatch_contact_attack(ctx);
}

void frog_controller::on_process_pending_attacks(Asset& self) {
    CustomAssetController::on_process_pending_attacks(self);
}
