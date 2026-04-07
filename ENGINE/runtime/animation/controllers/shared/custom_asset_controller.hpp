#pragma once
#include "animation/controllers/shared/child_asset.hpp"
#include "assets/asset/asset_controller.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Asset;
class Assets;
class Input;

namespace animation_update::custom_controllers {

class WanderControllerBehavior;

} // namespace animation_update::custom_controllers

// Base for engine custom controllers. Keeps a stable self pointer and routes
// engine callbacks into controller-specific hooks.
class CustomAssetController : public AssetController {
public:
    explicit CustomAssetController(Asset* self);
    ~CustomAssetController() override;

    void update(const Input& in) final;
    void process_pending_attacks(Asset& self) final;

protected:
    Asset* self_ptr() const { return self_; }
    Assets* assets() const;

    virtual void on_update(const Input& in);
    virtual void on_process_pending_attacks(Asset& self);

private:
    friend class animation_update::custom_controllers::WanderControllerBehavior;

    struct AnchorCandidateAttachment {
        std::string anchor_name;
        std::string resolved_asset_name;
        std::optional<ChildAsset> child;
        int remaining_spawn_retries = 0;
        bool exhausted = false;
    };

    void initialize_anchor_candidate_children();
    void tick_anchor_candidate_attachments();
    std::uint64_t anchor_candidate_hash(const std::string& anchor_name) const;
    std::string owner_identity_for_anchor_candidates() const;

    std::optional<ChildAsset> surface_child_;
    std::vector<AnchorCandidateAttachment> anchor_candidate_children_;
    Asset* self_ = nullptr;
};
