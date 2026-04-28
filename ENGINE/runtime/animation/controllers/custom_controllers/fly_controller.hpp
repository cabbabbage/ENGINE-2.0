#pragma once
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "animation/controllers/shared/random_orbit_3d_controller_behavior.hpp"

class Asset;
class Input;

class fly_controller : public CustomAssetController {
public:
    explicit fly_controller(Asset* self);
    ~fly_controller() override = default;
    bool orbiting = true;
protected:
    void on_update(const Input& in) override;
    void on_attack(const animation_update::Attack& attack) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    animation_update::custom_controllers::RandomOrbit3DControllerBehavior orbit_behavior_;

};
