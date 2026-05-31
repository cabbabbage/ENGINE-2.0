#pragma once
#include "animation/controllers/custom_controller.hpp"

class Asset;
class Input;

class fly_controller : public custom_controller_api::CustomControllerBase {
public:
    explicit fly_controller(Asset* self);
    ~fly_controller() override = default;
    bool orbiting = true;
protected:
    void on_update(const Input& in) override;
    void on_attack(const animation_update::Attack& attack) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    float orbit_angle_radians_ = 0.0f;

};
