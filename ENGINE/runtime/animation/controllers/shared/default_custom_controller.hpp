#pragma once

#include "animation/controllers/shared/custom_asset_controller.hpp"

namespace animation_update::custom_controllers {

// Canonical runtime contract for custom controller implementations.
// This class forwards every lifecycle hook to CustomAssetController defaults
// and exposes stable utility accessors for generated/manual controllers.
class DefaultCustomController : public ::CustomAssetController {
public:
    explicit DefaultCustomController(Asset* self, bool generic_fallback = false);
    ~DefaultCustomController() override;

protected:
    Asset* controller_self() const { return self_ptr(); }
    Assets* controller_assets() const { return assets(); }
    const ControllerGameContext& controller_game_context() const { return game_context(); }
    runtime::context::GameRuntimeContext* controller_runtime_game_context() const {
        return mutable_runtime_game_context();
    }

    void on_init() override;
    void on_update(const Input& in) override;
    void on_attack(const animation_update::Attack& attack) override;
    void on_hit(const animation_update::Attack& attack) override;
    void on_death() override;
    void on_no_pending_attacks() override;
    void on_after_attack() override;
    AttackProcessingConfig attack_processing_config() const override;
    void on_process_pending_attacks(Asset& self) override;
    void on_pre_delete_hook(Asset& self) override;
    void on_orphaned_hook(Asset& self,
                          Asset* former_parent,
                          std::optional<OrphanImpulse> impulse = std::nullopt) override;
    void on_interact_hook(Asset& self, Asset* instigator) override;
};

} // namespace animation_update::custom_controllers
