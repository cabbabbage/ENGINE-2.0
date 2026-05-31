#include "animation/controllers/shared/custom_controller_base.hpp"

#include "animation/controllers/shared/internal/controller_runtime_backend.hpp"

#include <SDL3/SDL.h>

#include <utility>

#include "animation/animation_tag_utils.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_types.hpp"

namespace animation_update::custom_controllers {

namespace {

bool should_use_auto_move_attack_dispatch(const Asset* self) {
    if (!self || !self->info) {
        return false;
    }
    if (asset_types::canonicalize(self->info->type) != asset_types::enemy) {
        return false;
    }
    for (const auto& [animation_id, animation] : self->info->animations) {
        (void)animation_id;
        if (animation_update::tag_utils::has_normalized_tag(animation.tags, "attack")) {
            return true;
        }
    }
    return false;
}

} // namespace

CustomControllerBase::CustomControllerBase(Asset* self, bool generic_fallback)
    : runtime_backend_(std::make_unique<internal::ControllerRuntimeBackend>(self, generic_fallback)),
      rng_(std::random_device{}()) {
    on_init();
}

CustomControllerBase::~CustomControllerBase() = default;

void CustomControllerBase::update(const Input& in) {
    runtime_backend_->begin_frame_update();
    on_update(in);
}

void CustomControllerBase::process_pending_attacks(Asset& self) {
    const auto pending_attacks = self.process_pending_attacks();
    if (pending_attacks.empty()) {
        on_no_pending_attacks();
        on_process_pending_attacks(self);
        return;
    }

    for (const auto& attack : pending_attacks) {
        on_attack(attack);
    }

    const auto summary = internal::ControllerCombatSystem::process_attacks(
        self,
        pending_attacks,
        attack_processing_config());

    if (summary.died) {
        runtime_backend_->orphan_eligible_children(self);
        on_death();
    } else if (summary.took_damage) {
        for (const auto& attack : pending_attacks) {
            if (attack.payload.damage_amount > 0 || attack.damage_amount > 0) {
                on_hit(attack);
                break;
            }
        }
    }

    on_process_pending_attacks(self);
}

bool CustomControllerBase::requires_runtime_update() const {
    return runtime_backend_->requires_runtime_update();
}

void CustomControllerBase::on_pre_delete(Asset& self) {
    runtime_backend_->on_pre_delete(self);
    on_pre_delete_hook(self);
}

void CustomControllerBase::on_orphaned(Asset& self,
                                       Asset* former_parent,
                                       std::optional<OrphanImpulse> impulse) {
    runtime_backend_->on_orphaned(self, former_parent, impulse);
    on_orphaned_hook(self, former_parent, impulse);
}

void CustomControllerBase::on_interact(Asset& self, Asset* instigator) {
    on_interact_hook(self, instigator);
}

Asset* CustomControllerBase::controller_self() const {
    return runtime_backend_->self();
}

Assets* CustomControllerBase::controller_assets() const {
    return runtime_backend_->assets();
}

const ControllerGameContext& CustomControllerBase::controller_game_context() const {
    return runtime_backend_->game_context();
}

runtime::context::GameRuntimeContext* CustomControllerBase::controller_runtime_game_context() const {
    return runtime_backend_->mutable_runtime_game_context();
}

void CustomControllerBase::on_init() {}

void CustomControllerBase::on_update(const Input&) {
    Asset* self = controller_self();
    if (self && self->info && self->anim_ && self->default_controller_animation_enforced()) {
        const std::string default_anim{animation_update::detail::kDefaultAnimation};

        auto it = self->info->animations.find(default_anim);
        if (it != self->info->animations.end() && it->second.has_frames()) {
            if (self->current_animation != default_anim || self->current_frame == nullptr) {
                self->anim_->move(SDL_Point{0, 0}, default_anim);
            }
        }
    }

    if (self &&
        !self->current_attack_box_volumes().empty() &&
        !should_use_auto_move_attack_dispatch(self)) {
        (void)apply_attack_hits_to_active_targets();
    }
}

void CustomControllerBase::on_attack(const animation_update::Attack&) {}

void CustomControllerBase::on_hit(const animation_update::Attack&) {}

void CustomControllerBase::on_death() {}

void CustomControllerBase::on_no_pending_attacks() {}

void CustomControllerBase::on_after_attack() {
    Asset* self = controller_self();
    Asset* player = resolve_target_player();
    if (!self || !player) {
        return;
    }
    MovementConfig config{};
    config.combat_overrides.attacking_enabled = false;
    (void)seek_target(*player, 220, config);
}

AttackProcessingConfig CustomControllerBase::attack_processing_config() const {
    return {};
}

void CustomControllerBase::on_process_pending_attacks(Asset&) {}

void CustomControllerBase::on_pre_delete_hook(Asset&) {}

void CustomControllerBase::on_orphaned_hook(Asset&,
                                            Asset*,
                                            std::optional<OrphanImpulse>) {}

void CustomControllerBase::on_interact_hook(Asset& self, Asset* instigator) {
    (void)instigator;
    if (!self.anim_ || !self.info) {
        return;
    }

    if (self.anim_->set_animation_by_tags({"interact"}, {})) {
        return;
    }
    const auto interact_animation = self.info->animations.find("interact");
    if (interact_animation != self.info->animations.end() && interact_animation->second.has_frames()) {
        self.anim_->set_animation("interact");
    }
}

Asset* CustomControllerBase::resolve_target_player() const {
    const auto& ctx = controller_game_context();
    return ctx.player_is_valid() ? ctx.resolved_player : nullptr;
}

bool CustomControllerBase::is_target_valid(const Asset* target) const {
    Asset* self = controller_self();
    return target && self && target != self && !target->dead && target->active;
}

bool CustomControllerBase::play_animation(const std::string& animation_id) {
    Asset* self = controller_self();
    if (!self || !self->anim_ || animation_id.empty()) {
        return false;
    }
    self->anim_->set_animation(animation_id);
    return true;
}

bool CustomControllerBase::play_animation_by_tags(const std::vector<std::string>& required_tags,
                                                  const std::vector<std::string>& excluded_tags) {
    Asset* self = controller_self();
    if (!self || !self->anim_) {
        return false;
    }
    return self->anim_->set_animation_by_tags_deterministic(required_tags, excluded_tags);
}

void CustomControllerBase::play_default_idle() {
    (void)play_animation(animation_update::detail::kDefaultAnimation);
}

void CustomControllerBase::reverse_current_animation_until_stop() {
    Asset* self = controller_self();
    if (self && self->anim_) {
        self->anim_->begin_reverse_current_animation_until_stop();
    }
}

void CustomControllerBase::reverse_current_animation_to_default() {
    Asset* self = controller_self();
    if (self && self->anim_) {
        self->anim_->begin_reverse_current_animation_to_default();
    }
}

void CustomControllerBase::stop_reverse_current_animation() {
    Asset* self = controller_self();
    if (self && self->anim_) {
        self->anim_->stop_reverse_current_animation();
    }
}

bool CustomControllerBase::move_3d(const axis::WorldPos& delta,
                                   const std::string& animation,
                                   bool override_non_locked) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::move_by_delta_3d(*self, delta, animation, override_non_locked)
                : false;
}

bool CustomControllerBase::move_toward(const axis::WorldPos& target,
                                       int step_px,
                                       const MovementConfig& config) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::move_toward_point(*self, target, step_px, config)
                : false;
}

bool CustomControllerBase::move_away(const axis::WorldPos& point,
                                     int step_px,
                                     const MovementConfig& config) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::move_away_from_point(*self, point, step_px, config)
                : false;
}

bool CustomControllerBase::seek_target(Asset& target,
                                       int desired_range_px,
                                       const MovementConfig& config) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::seek_target(*self, target, desired_range_px, config)
                : false;
}

bool CustomControllerBase::chase_target(Asset& target,
                                        const MovementConfig& config) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::chase_target(*self, target, config)
                : false;
}

bool CustomControllerBase::retreat_from_target(Asset& target,
                                               int retreat_distance_px,
                                               const MovementConfig& config) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::retreat_from_target(*self, target, retreat_distance_px, config)
                : false;
}

bool CustomControllerBase::patrol(const std::vector<axis::WorldPos>& points,
                                  const MovementConfig& config) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::patrol(*self, points, behavior_state_.patrol_state, config)
                : false;
}

bool CustomControllerBase::idle_wander(int min_delta_px,
                                       int max_delta_px,
                                       const MovementConfig& config) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::idle_wander(*self, rng_, min_delta_px, max_delta_px, config)
                : false;
}

bool CustomControllerBase::return_home(int threshold_px,
                                       const MovementConfig& config) {
    Asset* self = controller_self();
    return self ? internal::ControllerAgentSystem::tick_return_home(*self, behavior_state_, threshold_px, config)
                : false;
}

bool CustomControllerBase::face_target(Asset& target) {
    Asset* self = controller_self();
    if (!self) {
        return false;
    }
    const bool changed = internal::ControllerAgentSystem::face_target(*self, target);
    if (changed) {
        notify_anchor_changed();
    }
    return changed;
}

bool CustomControllerBase::face_direction(float dir_x, float dir_z, float pitch_radians) {
    Asset* self = controller_self();
    if (!self) {
        return false;
    }
    const bool changed =
        internal::ControllerAgentSystem::face_direction(*self, dir_x, dir_z, pitch_radians);
    if (changed) {
        notify_anchor_changed();
    }
    return changed;
}

void CustomControllerBase::run_enemy_behavior(Asset* target,
                                              const EnemyAgentConfig& config,
                                              const MovementConfig& chase_move,
                                              const MovementConfig& retreat_move) {
    Asset* self = controller_self();
    if (!self) {
        return;
    }
    internal::ControllerAgentSystem::tick_enemy_behavior(
        *self,
        target,
        behavior_state_,
        config,
        chase_move,
        retreat_move);
}

bool CustomControllerBase::run_wander_behavior(Asset* target,
                                               int idle_radius_px,
                                               int min_wander_delta_px,
                                               int max_wander_delta_px,
                                               const MovementConfig& config) {
    Asset* self = controller_self();
    if (!self) {
        return false;
    }
    return internal::ControllerAgentSystem::tick_wander(*self,
                                                           target,
                                                           behavior_state_,
                                                           rng_,
                                                           idle_radius_px,
                                                           min_wander_delta_px,
                                                           max_wander_delta_px,
                                                           config);
}

bool CustomControllerBase::start_attack(const std::string& animation_id,
                                        const std::vector<std::string>& required_tags,
                                        const std::vector<std::string>& excluded_tags) {
    Asset* self = controller_self();
    return self ? internal::ControllerCombatSystem::start_attack_animation(
                      *self,
                      animation_id,
                      required_tags,
                      excluded_tags)
                : false;
}

bool CustomControllerBase::is_target_in_range(Asset& target, int range_px) const {
    Asset* self = controller_self();
    return self ? internal::ControllerCombatSystem::is_target_in_range(*self, target, range_px) : false;
}

bool CustomControllerBase::cooldown_ready(const std::string& cooldown_key) const {
    return internal::ControllerCombatSystem::cooldown_ready(cooldown_state_, cooldown_key);
}

void CustomControllerBase::start_cooldown(const std::string& cooldown_key, float seconds) {
    internal::ControllerCombatSystem::start_cooldown(cooldown_state_, cooldown_key, seconds);
}

bool CustomControllerBase::try_attack_target(Asset& target,
                                             const std::string& cooldown_key,
                                             float cooldown_seconds,
                                             int range_px,
                                             const std::string& attack_animation,
                                             const std::vector<std::string>& required_tags,
                                             const std::vector<std::string>& excluded_tags) {
    Asset* self = controller_self();
    return self ? internal::ControllerCombatSystem::try_attack_target(*self,
                                                                       target,
                                                                       cooldown_state_,
                                                                       cooldown_key,
                                                                       cooldown_seconds,
                                                                       range_px,
                                                                       attack_animation,
                                                                       required_tags,
                                                                       excluded_tags)
                : false;
}

bool CustomControllerBase::apply_attack_hit(Asset& target) {
    Asset* self = controller_self();
    return self ? internal::ControllerCombatSystem::apply_attack_hit(*self, target) : false;
}

bool CustomControllerBase::apply_attack_hits_to_active_targets() {
    Asset* self = controller_self();
    return self ? internal::ControllerCombatSystem::apply_attack_hits_to_active_targets(*self, controller_assets())
                : false;
}

bool CustomControllerBase::is_hit_window_open(int start_frame, int end_frame) const {
    Asset* self = controller_self();
    return self ? internal::ControllerCombatSystem::is_hit_window_open(*self, start_frame, end_frame)
                : false;
}

ChildAsset* CustomControllerBase::spawn_bind_child(const std::string& key,
                                                   const std::string& asset_name,
                                                   const std::string& anchor_name,
                                                   bool hidden) {
    Asset* self = controller_self();
    if (!self || key.empty() || asset_name.empty() || anchor_name.empty()) {
        return nullptr;
    }

    auto it = controller_children_.find(key);
    if (it == controller_children_.end()) {
        auto child = std::make_unique<ChildAsset>(*self, asset_name);
        child->bind(anchor_name);
        if (hidden) {
            child->hide();
        } else {
            child->unhide();
        }
        auto [inserted_it, inserted] = controller_children_.emplace(key, std::move(child));
        (void)inserted;
        return inserted_it->second.get();
    }

    it->second->bind(anchor_name);
    if (hidden) {
        it->second->hide();
    } else {
        it->second->unhide();
    }
    return it->second.get();
}

ChildAsset* CustomControllerBase::child_helper(const std::string& key) {
    const auto it = controller_children_.find(key);
    return it != controller_children_.end() ? it->second.get() : nullptr;
}

Asset* CustomControllerBase::child_asset(const std::string& key) {
    if (ChildAsset* helper = child_helper(key)) {
        return helper->get_asset();
    }
    return nullptr;
}

Asset* CustomControllerBase::orphan_child(const std::string& key,
                                          std::optional<OrphanImpulse> impulse) {
    auto it = controller_children_.find(key);
    if (it == controller_children_.end()) {
        return nullptr;
    }
    return it->second->orphan(impulse);
}

void CustomControllerBase::destroy_child(const std::string& key) {
    controller_children_.erase(key);
}

void CustomControllerBase::clear_all_children() {
    controller_children_.clear();
}

void CustomControllerBase::notify_anchor_changed(const std::string& anchor_name) {
    Asset* self = controller_self();
    if (!self) {
        return;
    }
    anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(self, anchor_name);
}

} // namespace animation_update::custom_controllers

