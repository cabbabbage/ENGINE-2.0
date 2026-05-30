#include "small_spider_controller.hpp"

#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include <algorithm>

small_spider_controller::small_spider_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    behavior_config_.kamikaze = false;
    behavior_config_.ranges.aggro_radius_px = 120;
    behavior_config_.ranges.desired_standoff_px = 3;
    behavior_config_.ranges.attack_radius_px = 16;
    behavior_config_.retreat_distance_px = 140;
    behavior_config_.recover_ms = 210;
    behavior_config_.attack_window_ms = 160;
    behavior_config_.return_home_threshold_px = 70;
    behavior_config_.force_attacking_enabled = true;
    behavior_config_.airborne_buffer_px = 1;

    chase_move_.visit_threshold_px = 10;
    chase_move_.override_non_locked = false;
    chase_move_.allow_vertical_movement = false;
    retreat_move_.visit_threshold_px = 10;
    retreat_move_.override_non_locked = false;
    retreat_move_.allow_vertical_movement = false;

    Asset* owner = controller_self();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
        owner->set_default_controller_animation_enforced(false);
    }
}

void small_spider_controller::on_update(const Input& in) {
    custom_controller_api::CustomControllerBase::on_update(in);

    Asset* self = controller_self();
    const auto& ctx = controller_game_context();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = resolve_target_player();
    if (!player) {
        return;
    }

    if (behavior_config_.require_ground_contact) {
        if (Assets* owner_assets = self->get_assets()) {
            const world::GridPoint floor_point =
                owner_assets->resolve_floor_world_point(SDL_Point{self->world_x(), self->world_z()}, self->grid_resolution);
            const int airborne_buffer_px = std::max(0, behavior_config_.airborne_buffer_px);
            if (self->world_y() > floor_point.world_y() + airborne_buffer_px) {
                return;
            }
        } else {
            return;
        }
    }

    run_enemy_behavior(player, behavior_config_, chase_move_, retreat_move_);
}
