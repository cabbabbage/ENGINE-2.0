#ifndef SPIDER_EGG_CONTROLLER_HPP
#define SPIDER_EGG_CONTROLLER_HPP

#include "animation/controllers/shared/custom_controller_api.hpp"
#include <optional>

class Asset;
class Input;

class spider_egg_controller : public custom_controller_api::DefaultCustomController {

public:
    explicit spider_egg_controller(Asset* self);
    ~spider_egg_controller() override = default;

protected:
    void on_init() override;
    void on_update(const Input& in) override;
    void on_attack(const animation_update::Attack& attack) override;
    void on_hit(const animation_update::Attack& attack) override;
    void on_death() override;
    void on_no_pending_attacks() override;
    void on_after_attack() override;
    custom_controller_api::AttackProcessingConfig attack_processing_config() const override;
    void on_orphaned_hook(Asset& self,
                          Asset* former_parent,
                          std::optional<OrphanImpulse> impulse = std::nullopt) override;
    void on_pre_delete_hook(Asset& self) override;
    void on_process_pending_attacks(Asset& self) override;
    void on_interact_hook(Asset& self, Asset* instigator) override;
};

#endif
