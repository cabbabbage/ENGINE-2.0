#ifndef CHEST_OPENING_CONTROLLER_HPP
#define CHEST_OPENING_CONTROLLER_HPP

#include "animation/controllers/custom_controller.hpp"
#include <optional>

class Asset;
class Input;

class chest_opening_controller : public custom_controller_api::CustomControllerBase {

public:
    explicit chest_opening_controller(Asset* self);
    ~chest_opening_controller() override = default;

protected:
    void on_init() override;
    void on_update(const Input& in) override;
    void on_attack(const animation_update::Attack& attack) override;
    void on_hit(const animation_update::Attack& attack) override;
    void on_death() override;
    void on_no_pending_attacks() override;
    void on_orphaned_hook(Asset& self,
                          Asset* former_parent,
                          std::optional<OrphanImpulse> impulse = std::nullopt) override;
    void on_pre_delete_hook(Asset& self) override;
    void on_process_pending_attacks(Asset& self) override;
    void on_interact_hook(Asset& self, Asset* instigator) override;
};

#endif
