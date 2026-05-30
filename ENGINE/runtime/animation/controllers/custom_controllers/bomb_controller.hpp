#ifndef bomb_CONTROLLER_HPP
#define bomb_CONTROLLER_HPP

#include "animation/controllers/custom_controller.hpp"

class Asset;
class Input;

class bomb_controller : public custom_controller_api::CustomControllerBase {

public:
    explicit bomb_controller(Asset* self);
    ~bomb_controller() override = default;

protected:
    void on_update(const Input&) override;
    void on_death() override;
    void on_process_pending_attacks(Asset& self) override;

private:
    bool can_detonate(const Asset& self, const Asset& target) const;
    void detonate(Asset& self, Asset& target);
    void dispatch_explosion_attacks(Asset& self, Asset* primary_target);

    custom_controller_api::EnemyAgentConfig behavior_config_{};
    custom_controller_api::MovementConfig chase_move_{};
    custom_controller_api::MovementConfig retreat_move_{};
    bool has_detonated_ = false;
};

#endif
