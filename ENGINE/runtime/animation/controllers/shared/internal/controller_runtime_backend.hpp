#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "animation/controllers/shared/child_asset.hpp"
#include "animation/controllers/shared/controller_game_context.hpp"

class Asset;
class Assets;

namespace runtime::context {
class GameRuntimeContext;
}

namespace animation_update::custom_controllers::internal {

class ControllerRuntimeBackend {
public:
    explicit ControllerRuntimeBackend(Asset* self, bool generic_fallback = false);

    bool requires_runtime_update() const;
    void begin_frame_update();

    Asset* self() const { return self_; }
    Assets* assets() const;
    const ControllerGameContext& game_context() const { return game_context_; }
    runtime::context::GameRuntimeContext* mutable_runtime_game_context() const;

    void orphan_eligible_children(Asset& owner);
    void on_pre_delete(Asset& owner);
    void on_orphaned(Asset& self,
                     Asset* former_parent,
                     std::optional<OrphanImpulse> impulse = std::nullopt);

private:
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
        double world_x = 0.0;
        double world_z = 0.0;
        int resolution_layer = 0;
        double world_y = 0.0;
        double floor_y = 0.0;
        double velocity_x = 0.0;
        double velocity_z = 0.0;
        double velocity_y = 0.0;
        double restitution = 0.0;
    };

    void initialize_anchor_candidate_children();
    void tick_anchor_candidate_attachments();
    void tick_orphan_fall_state();
    std::uint64_t anchor_candidate_hash(const std::string& anchor_name) const;
    std::string owner_identity_for_anchor_candidates() const;

    std::vector<AnchorCandidateAttachment> anchor_candidate_children_;
    FlyOrbitTargetSnapshot fly_orbit_target_state_{};
    ControllerGameContext game_context_{};
    OrphanFallState orphan_fall_state_{};
    Asset* self_ = nullptr;
    bool generic_fallback_ = false;
};

} // namespace animation_update::custom_controllers::internal
