#pragma once
#include "animation/controllers/shared/child_asset.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "animation/controllers/shared/controller_game_context.hpp"
#include "assets/asset/asset_controller.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Asset;
class Assets;
class Input;
namespace animation_update {
struct Attack;
}
namespace runtime::context {
class GameRuntimeContext;
}

namespace animation_update::custom_controllers {

class WanderControllerBehavior;
class RandomOrbit3DControllerBehavior;

} // namespace animation_update::custom_controllers

// Base for engine custom controllers. Keeps a stable self pointer and routes
// engine callbacks into controller-specific hooks.
class CustomAssetController : public AssetController {
public:
    explicit CustomAssetController(Asset* self);
    ~CustomAssetController() override;

    void update(const Input& in) final;
    void process_pending_attacks(Asset& self) final;
    void on_pre_delete(Asset& self) final;
    void on_orphaned(Asset& self, Asset* former_parent) override;

protected:
    Asset* self_ptr() const { return self_; }
    Assets* assets() const;
    const animation_update::custom_controllers::ControllerGameContext& game_context() const { return game_context_; }
    runtime::context::GameRuntimeContext* mutable_runtime_game_context() const;

    virtual void on_init();
    virtual void on_update(const Input& in);
    virtual void on_attack(const animation_update::Attack& attack);
    virtual void on_hit(const animation_update::Attack& attack);
    virtual void on_death();
    virtual void on_no_pending_attacks();
    virtual animation_update::custom_controllers::AttackProcessingConfig attack_processing_config() const;
    virtual void on_process_pending_attacks(Asset& self);
    virtual void on_pre_delete_hook(Asset& self);
    virtual void on_orphaned_hook(Asset& self, Asset* former_parent);

private:
    friend class animation_update::custom_controllers::WanderControllerBehavior;
    friend class animation_update::custom_controllers::RandomOrbit3DControllerBehavior;

    struct AnchorCandidateAttachment {
        std::string anchor_name;
        std::string resolved_asset_name;
        std::optional<ChildAsset> child;
        int remaining_spawn_retries = 0;
        bool exhausted = false;
        bool orphan_on_end = true;
        bool orphaned = false;
    };

    struct OrphanFallState {
        bool active = false;
        int world_x = 0;
        int world_z = 0;
        int resolution_layer = 0;
        double world_y = 0.0;
        double floor_y = 0.0;
        double velocity_y = 0.0;
        double restitution = 0.0;
    };

    void initialize_anchor_candidate_children();
    void tick_anchor_candidate_attachments();
    void orphan_eligible_children(Asset& owner);
    void tick_orphan_fall_state();
    std::uint64_t anchor_candidate_hash(const std::string& anchor_name) const;
    std::string owner_identity_for_anchor_candidates() const;

    std::optional<ChildAsset> surface_child_;
    std::vector<AnchorCandidateAttachment> anchor_candidate_children_;
    animation_update::custom_controllers::FlyOrbitTargetSnapshot fly_orbit_target_state_{};
    animation_update::custom_controllers::ControllerGameContext game_context_{};
    OrphanFallState orphan_fall_state_{};
    Asset* self_ = nullptr;
};
