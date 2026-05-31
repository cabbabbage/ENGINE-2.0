#include "spider_egg_controller.hpp"

#include "animation/animation_update.hpp"
#include "animation/animation_tag_utils.hpp"
#include "animation/attack.hpp"
#include "animation/controllers/shared/child_asset.hpp"
#include "assets/asset/Asset.hpp"
#include "core/game_runtime_context.hpp"
#include "utils/range_util.hpp"
#include <random>

namespace {

constexpr const char* kEggBreakTag = "break";
constexpr const char* kEggBreakAnimationFallback = "end";
constexpr const char* kSmallSpiderAssetName = "small_spider";
constexpr const char* kSpiderChildAnchorName = "spider_child";
constexpr const char* kSmallSpiderAttackerName = "small_spider";
constexpr double kDroppedEggCrackChance = 0.5;
constexpr int kSprintDashEggDisturbRadiusPx = 75;
constexpr double kNearbySprintDashEggCrackChance = 0.5;

bool roll_chance(double probability) {
    if (probability <= 0.0) {
        return false;
    }
    if (probability >= 1.0) {
        return true;
    }
    thread_local std::mt19937 rng{std::random_device{}()};
    std::bernoulli_distribution dist(probability);
    return dist(rng);
}

bool animation_has_tag(const Asset& asset, const char* tag) {
    if (!asset.info || asset.current_animation.empty()) {
        return false;
    }
    const auto it = asset.info->animations.find(asset.current_animation);
    if (it == asset.info->animations.end()) {
        return false;
    }
    return animation_update::tag_utils::has_normalized_tag(it->second.tags, tag);
}

} // namespace

spider_egg_controller::spider_egg_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {}

void spider_egg_controller::on_init() {
    custom_controller_api::CustomControllerBase::on_init();
}

void spider_egg_controller::on_update(const Input& in) {
    custom_controller_api::CustomControllerBase::on_update(in);
    (void)in;

    Asset* self = controller_self();
    if (!self || !self->anim_ || !is_cracking_) {
        if (self) {
            process_player_motion_disturbance(*self);
        }
        return;
    }

    if (!self->is_current_animation_last_frame()) {
        return;
    }
    if (animation_has_tag(*self, kEggBreakTag) || self->current_animation == kEggBreakAnimationFallback) {
        self->Delete();
    }
}

void spider_egg_controller::on_attack(const animation_update::Attack& attack) {
    custom_controller_api::CustomControllerBase::on_attack(attack);

    Asset* self = controller_self();
    if (!self || !self->anim_ || is_cracking_) {
        return;
    }
    if (attack.attacker_asset_name == kSmallSpiderAttackerName) {
        return;
    }

    (void)try_start_cracking(*self);
}

void spider_egg_controller::on_hit(const animation_update::Attack& attack) {
    custom_controller_api::CustomControllerBase::on_hit(attack);
    (void)attack;
}

void spider_egg_controller::on_death() {
    custom_controller_api::CustomControllerBase::on_death();
}

void spider_egg_controller::on_no_pending_attacks() {
    custom_controller_api::CustomControllerBase::on_no_pending_attacks();
}

void spider_egg_controller::on_after_attack() {}

custom_controller_api::AttackProcessingConfig spider_egg_controller::attack_processing_config() const {
    return custom_controller_api::CustomControllerBase::attack_processing_config();
}

void spider_egg_controller::on_orphaned_hook(Asset& self,
                                             Asset* former_parent,
                                             std::optional<OrphanImpulse> impulse) {
    custom_controller_api::CustomControllerBase::on_orphaned_hook(self, former_parent, impulse);
    if (is_cracking_) {
        return;
    }
    if (const auto* runtime_ctx = controller_game_context().shared) {
        if (former_parent && former_parent == runtime_ctx->player() &&
            runtime_ctx->player_motion_disturbance().active) {
            (void)try_start_cracking(self);
            return;
        }
    }
    if (roll_chance(kDroppedEggCrackChance)) {
        (void)try_start_cracking(self);
    }
}

void spider_egg_controller::on_pre_delete_hook(Asset& self) {
    custom_controller_api::CustomControllerBase::on_pre_delete_hook(self);
}

void spider_egg_controller::on_process_pending_attacks(Asset& self_ref) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self_ref);
}

void spider_egg_controller::on_interact_hook(Asset& self, Asset* instigator) {
    custom_controller_api::CustomControllerBase::on_interact_hook(self, instigator);
}

void spider_egg_controller::spawn_small_spider_child(Asset& self) {
    if (spawned_child_) {
        return;
    }

    ChildAsset child(self, kSmallSpiderAssetName);
    child.bind(kSpiderChildAnchorName);
    (void)child.update();
    Asset* orphaned = child.orphan();
    if (!orphaned) {
        return;
    }
    spawned_child_ = true;
}

bool spider_egg_controller::try_start_cracking(Asset& self) {
    if (is_cracking_ || !self.anim_) {
        return false;
    }
    is_cracking_ = true;
    if (!self.anim_->set_animation_by_tags({kEggBreakTag}, {})) {
        self.anim_->set_animation(kEggBreakAnimationFallback);
    }
    spawn_small_spider_child(self);
    return true;
}

void spider_egg_controller::process_player_motion_disturbance(Asset& self) {
    const auto* runtime_ctx = controller_game_context().shared;
    if (!runtime_ctx || !runtime_ctx->player()) {
        return;
    }

    const runtime::context::PlayerMotionDisturbanceState& disturbance =
        runtime_ctx->player_motion_disturbance();
    if (!disturbance.active || disturbance.pulse_id == 0) {
        return;
    }
    if (disturbance.pulse_id == last_processed_player_disturbance_pulse_) {
        return;
    }
    last_processed_player_disturbance_pulse_ = disturbance.pulse_id;

    const long long radius_sq =
        static_cast<long long>(kSprintDashEggDisturbRadiusPx) * static_cast<long long>(kSprintDashEggDisturbRadiusPx);
    if (Range::distance_sq(runtime_ctx->player(), &self) > radius_sq) {
        return;
    }
    if (roll_chance(kNearbySprintDashEggCrackChance)) {
        (void)try_start_cracking(self);
    }
}
