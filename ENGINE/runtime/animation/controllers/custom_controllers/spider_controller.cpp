#include "spider_controller.hpp"

#include "assets/asset/Asset.hpp"
#include "animation/controllers/shared/enemy_archetype_controller.hpp"
#include "utils/log.hpp"
#include <optional>

spider_controller::spider_controller(Asset* self)
    : custom_controller_api::CustomControllerBase(self) {
    const auto preset = custom_controller_api::enemy_archetypes::EnemyArchetypePresets::spider();
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

void spider_controller::on_update(const Input& in) {
    custom_controller_api::CustomControllerBase::on_update(in);
    const auto& ctx = controller_game_context();
    Asset* self = ctx.self;
    if (!self || !self->anim_ || !ctx.has_assets()) {
        return;
    }

    Asset* player = resolve_target_player();
    if (player && !ctx.self_and_player_share_room()) {
        player = nullptr;
    }
    if (!player && self->anim_->debug_enabled()) {
        vibble::log::info("[AICombat] Spider could not acquire player target");
    }
    custom_controller_api::enemy_ai::EnemyAiPlanConfig ai_config{};
    ai_config.behavior = behavior_config_;
    ai_config.approach_move = chase_move_;
    ai_config.retreat_move = retreat_move_;
    last_ai_frame_ = custom_controller_api::enemy_ai::LegacyEnemyAiAdapter::tick(
        *self,
        player,
        player != nullptr,
        behavior_state(),
        ai_config);
    if (player && last_ai_frame_.attack.manual_attack_allowed) {
        (void)face_target(*player);
        const auto profile = custom_controller_api::enemy_archetypes::EnemyArchetypePresets::spider().primary_attack;
        (void)try_attack_target(*player,
                                profile.id,
                                static_cast<float>(profile.cooldown_ms_on_start) / 1000.0f,
                                profile.max_range_px,
                                profile.animation_id,
                                profile.required_tags,
                                profile.excluded_tags);
    }
}

void spider_controller::on_process_pending_attacks(Asset& self) {
    custom_controller_api::CustomControllerBase::on_process_pending_attacks(self);
}

