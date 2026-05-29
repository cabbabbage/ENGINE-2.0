#include "small_spider_controller.hpp"

#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

small_spider_controller::small_spider_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    behavior_config_.kamikaze = false;
    behavior_config_.chase_range_px = 34;
    behavior_config_.attack_range_px = 16;
    behavior_config_.retreat_distance_px = 140;
    behavior_config_.recover_ms = 210;
    behavior_config_.return_home_threshold_px = 70;
    behavior_config_.force_attacking_enabled = true;

    chase_move_.visit_threshold_px = 10;
    chase_move_.override_non_locked = false;
    retreat_move_.visit_threshold_px = 10;
    retreat_move_.override_non_locked = false;

    Asset* owner = controller_self();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
        owner->set_default_controller_animation_enforced(false);
    }
}

void small_spider_controller::on_init() {
    custom_controller_api::CustomControllerBase::on_init();
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

    if (Assets* owner_assets = self->get_assets()) {
        const world::GridPoint floor_point =
            owner_assets->resolve_floor_world_point(SDL_Point{self->world_x(), self->world_z()}, self->grid_resolution);
        constexpr int kAirbornePursuitBufferPx = 1;
        if (self->world_y() > floor_point.world_y() + kAirbornePursuitBufferPx) {
            return;
        }
    }

    run_enemy_behavior(player, behavior_config_, chase_move_, retreat_move_);
    apply_attack_hit(*player);
}

void small_spider_controller::on_attack(const animation_update::Attack& attack) {
    custom_controller_api::CustomControllerBase::on_attack(attack);
}

void small_spider_controller::on_hit(const animation_update::Attack& attack) {
    custom_controller_api::CustomControllerBase::on_hit(attack);
}

void small_spider_controller::on_death() {
    custom_controller_api::CustomControllerBase::on_death();
}

void small_spider_controller::on_no_pending_attacks() {
    custom_controller_api::CustomControllerBase::on_no_pending_attacks();
}

void small_spider_controller::on_after_attack() {
    custom_controller_api::CustomControllerBase::on_after_attack();
}

custom_controller_api::AttackProcessingConfig small_spider_controller::attack_processing_config() const {
    return custom_controller_api::CustomControllerBase::attack_processing_config();
}

void small_spider_controller::on_orphaned_hook(Asset& self,
                                               Asset* former_parent,
                                               std::optional<OrphanImpulse> impulse) {
    custom_controller_api::CustomControllerBase::on_orphaned_hook(self, former_parent, impulse);
}

void small_spider_controller::on_pre_delete_hook(Asset& self) {
    custom_controller_api::CustomControllerBase::on_pre_delete_hook(self);
}

void small_spider_controller::on_process_pending_attacks(Asset& self_ref) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self_ref);
}

void small_spider_controller::on_interact_hook(Asset& self, Asset* instigator) {
    custom_controller_api::CustomControllerBase::on_interact_hook(self, instigator);
}
