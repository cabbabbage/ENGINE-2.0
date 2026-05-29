#ifndef spider_CONTROLLER_HPP
#define spider_CONTROLLER_HPP

#include "animation/controllers/custom_controller.hpp"

class Asset;
class Input;

class spider_controller : public custom_controller_api::CustomControllerBase {

public:
    explicit spider_controller(Asset* self);
    ~spider_controller() override = default;

protected:
    void on_update(const Input&) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    custom_controller_api::EnemyBehaviorConfig behavior_config_{};
    custom_controller_api::MovementConfig chase_move_{};
    custom_controller_api::MovementConfig retreat_move_{};
};

#endif
