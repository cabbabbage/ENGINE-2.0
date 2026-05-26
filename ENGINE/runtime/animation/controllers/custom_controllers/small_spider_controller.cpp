#include "small_spider_controller.hpp"

#include "animation/animation_update.hpp"
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

small_spider_controller::small_spider_controller(Asset* self)
    : custom_controller_api::DefaultCustomController(self),
      steering_(custom_controller_api::EnemyCombatSteeringConfig{
          175,
          84,
          8,
          14,
          260,
          900
      }),
      behavior_(custom_controller_api::EnemyAutoCombatConfig{
          custom_controller_api::EnemyAutoCombatMode::SkirmisherShortEvade,
          34,
          16,
          140,
          70,
          210,
          true
      }) {
    Asset* owner = controller_self();
    if (owner && owner->anim_) {
        owner->anim_->set_debug_enabled(false);
        owner->needs_target = true;
        owner->set_default_controller_animation_enforced(false);
    }
}

void small_spider_controller::on_init() {
    custom_controller_api::DefaultCustomController::on_init();
}

void small_spider_controller::on_update(const Input& in) {
    custom_controller_api::DefaultCustomController::on_update(in);
    (void)in;

    Asset* self = controller_self();
    const auto& ctx = game_context();
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = custom_controller_api::resolve_valid_player_target(ctx);
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

    steering_.tick_progress(*self);
    behavior_.tick(*self, *player, steering_);

    // Continuous contact damage while overlap persists.
    custom_controller_api::dispatch_contact_attack(ctx);
}

void small_spider_controller::on_attack(const animation_update::Attack& attack) {
    custom_controller_api::DefaultCustomController::on_attack(attack);
}

void small_spider_controller::on_hit(const animation_update::Attack& attack) {
    custom_controller_api::DefaultCustomController::on_hit(attack);
}

void small_spider_controller::on_death() {
    custom_controller_api::DefaultCustomController::on_death();
}

void small_spider_controller::on_no_pending_attacks() {
    custom_controller_api::DefaultCustomController::on_no_pending_attacks();
}

void small_spider_controller::on_after_attack() {
    custom_controller_api::DefaultCustomController::on_after_attack();
}

custom_controller_api::AttackProcessingConfig small_spider_controller::attack_processing_config() const {
    return custom_controller_api::DefaultCustomController::attack_processing_config();
}

void small_spider_controller::on_orphaned_hook(Asset& self,
                                               Asset* former_parent,
                                               std::optional<OrphanImpulse> impulse) {
    custom_controller_api::DefaultCustomController::on_orphaned_hook(self, former_parent, impulse);
}

void small_spider_controller::on_pre_delete_hook(Asset& self) {
    custom_controller_api::DefaultCustomController::on_pre_delete_hook(self);
}

void small_spider_controller::on_process_pending_attacks(Asset& self_ref) {
    custom_controller_api::DefaultCustomController::on_process_pending_attacks(self_ref);
}

void small_spider_controller::on_interact_hook(Asset& self, Asset* instigator) {
    custom_controller_api::DefaultCustomController::on_interact_hook(self, instigator);
}
