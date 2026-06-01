#include "small_spider_controller.hpp"

#include "animation/animation_update.hpp"
#include "animation/controllers/shared/enemy_archetype_controller.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"
#include <algorithm>

small_spider_controller::small_spider_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    const auto preset = custom_controller_api::enemy_archetypes::EnemyArchetypePresets::small_spider();
    behavior_config_ = preset.behavior;
    chase_move_ = preset.approach_move;
    retreat_move_ = preset.retreat_move;

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
        if (self->anim_->debug_enabled()) {
            vibble::log::info("[AICombat] Small spider could not acquire player target");
        }
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
