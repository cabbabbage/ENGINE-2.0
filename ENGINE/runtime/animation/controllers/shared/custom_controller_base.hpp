#pragma once

#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "animation/animation_update.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "animation/controllers/shared/child_asset.hpp"
#include "animation/controllers/shared/controller_game_context.hpp"
#include "animation/controllers/shared/internal/controller_behavior_system.hpp"
#include "animation/controllers/shared/internal/controller_combat_system.hpp"
#include "animation/controllers/shared/internal/controller_movement_system.hpp"
#include "assets/asset/asset_controller.hpp"

class Asset;
class Assets;
class Input;

namespace runtime::context {
class GameRuntimeContext;
}

namespace animation_update {
struct Attack;
}

namespace animation_update::custom_controllers {

namespace internal {
class ControllerRuntimeBackend;
}

class CustomControllerBase : public AssetController {
public:
    explicit CustomControllerBase(Asset* self, bool generic_fallback = false);
    ~CustomControllerBase() override;

    void update(const Input& in) final;
    void process_pending_attacks(Asset& self) final;
    bool requires_runtime_update() const override;
    void on_pre_delete(Asset& self) final;
    void on_orphaned(Asset& self,
                     Asset* former_parent,
                     std::optional<OrphanImpulse> impulse = std::nullopt) override;
    void on_interact(Asset& self, Asset* instigator) final;

protected:
    Asset* controller_self() const;
    Assets* controller_assets() const;
    const ControllerGameContext& controller_game_context() const;
    runtime::context::GameRuntimeContext* controller_runtime_game_context() const;

    virtual void on_init();
    virtual void on_update(const Input& in);
    virtual void on_attack(const animation_update::Attack& attack);
    virtual void on_hit(const animation_update::Attack& attack);
    virtual void on_death();
    virtual void on_no_pending_attacks();
    virtual void on_after_attack();
    virtual AttackProcessingConfig attack_processing_config() const;
    virtual void on_process_pending_attacks(Asset& self);
    virtual void on_pre_delete_hook(Asset& self);
    virtual void on_orphaned_hook(Asset& self,
                                  Asset* former_parent,
                                  std::optional<OrphanImpulse> impulse = std::nullopt);
    virtual void on_interact_hook(Asset& self, Asset* instigator);

    // Targeting + context
    Asset* resolve_target_player() const;
    bool is_target_valid(const Asset* target) const;

    // Animation helpers
    bool play_animation(const std::string& animation_id);
    bool play_animation_by_tags(const std::vector<std::string>& required_tags,
                                const std::vector<std::string>& excluded_tags = {});
    void play_default_idle();
    void reverse_current_animation_until_stop();
    void reverse_current_animation_to_default();
    void stop_reverse_current_animation();

    // 3D movement helpers
    bool move_3d(const axis::WorldPos& delta,
                 const std::string& animation = animation_update::detail::kDefaultAnimation,
                 bool override_non_locked = true);
    bool move_toward(const axis::WorldPos& target,
                     int step_px,
                     const internal::MovementConfig& config = {});
    bool move_away(const axis::WorldPos& point,
                   int step_px,
                   const internal::MovementConfig& config = {});
    bool seek_target(Asset& target,
                     int desired_range_px,
                     const internal::MovementConfig& config = {});
    bool chase_target(Asset& target,
                      const internal::MovementConfig& config = {});
    bool retreat_from_target(Asset& target,
                             int retreat_distance_px,
                             const internal::MovementConfig& config = {});
    bool patrol(const std::vector<axis::WorldPos>& points,
                const internal::MovementConfig& config = {});
    bool idle_wander(int min_delta_px,
                     int max_delta_px,
                     const internal::MovementConfig& config = {});
    bool return_home(int threshold_px,
                     const internal::MovementConfig& config = {});
    bool face_target(Asset& target);
    bool face_direction(float dir_x, float dir_z, float pitch_radians = 0.0f);

    // Behavior helpers
    void run_enemy_behavior(Asset* target,
                            const internal::EnemyBehaviorConfig& config,
                            const internal::MovementConfig& chase_move = {},
                            const internal::MovementConfig& retreat_move = {});
    bool run_wander_behavior(Asset* target,
                             int idle_radius_px,
                             int min_wander_delta_px,
                             int max_wander_delta_px,
                             const internal::MovementConfig& config = {});

    // Combat helpers
    bool start_attack(const std::string& animation_id,
                      const std::vector<std::string>& required_tags = {},
                      const std::vector<std::string>& excluded_tags = {});
    bool is_target_in_range(Asset& target, int range_px) const;
    bool cooldown_ready(const std::string& cooldown_key) const;
    void start_cooldown(const std::string& cooldown_key, float seconds);
    bool try_attack_target(Asset& target,
                           const std::string& cooldown_key,
                           float cooldown_seconds,
                           int range_px,
                           const std::string& attack_animation = {},
                           const std::vector<std::string>& required_tags = {},
                           const std::vector<std::string>& excluded_tags = {});
    bool apply_attack_hit(Asset& target);
    bool apply_attack_hits_to_active_targets();
    bool is_hit_window_open(int start_frame, int end_frame) const;

    // Child + anchor helpers
    ChildAsset* spawn_bind_child(const std::string& key,
                                 const std::string& asset_name,
                                 const std::string& anchor_name,
                                 bool hidden = false);
    ChildAsset* child_helper(const std::string& key);
    Asset* child_asset(const std::string& key);
    Asset* orphan_child(const std::string& key,
                        std::optional<OrphanImpulse> impulse = std::nullopt);
    void destroy_child(const std::string& key);
    void clear_all_children();
    void notify_anchor_changed(const std::string& anchor_name = std::string{});

    internal::BehaviorState& behavior_state() { return behavior_state_; }
    const internal::BehaviorState& behavior_state() const { return behavior_state_; }

private:
    std::unique_ptr<internal::ControllerRuntimeBackend> runtime_backend_;
    internal::ControllerCombatSystem::CooldownState cooldown_state_{};
    internal::BehaviorState behavior_state_{};
    std::mt19937 rng_{};
    std::unordered_map<std::string, std::unique_ptr<ChildAsset>> controller_children_;
};

} // namespace animation_update::custom_controllers
