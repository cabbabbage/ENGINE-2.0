#ifndef BONESKI_CONTROLLER_HPP
#define BONESKI_CONTROLLER_HPP

#include "animation/controllers/custom_controller.hpp"

class Asset;
class Input;

class boneski_controller : public custom_controller_api::CustomControllerBase {

public:
    explicit boneski_controller(Asset* self);
    ~boneski_controller() override = default;

protected:
    void on_update(const Input& in) override;

private:
    custom_controller_api::EnemyAgentConfig behavior_config_{};
    custom_controller_api::MovementConfig chase_move_{};
    custom_controller_api::MovementConfig retreat_move_{};
};

#endif
