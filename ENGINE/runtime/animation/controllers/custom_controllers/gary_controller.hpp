#pragma once
#include "animation/controllers/shared/custom_controller_api.hpp"
#include "animation/controllers/shared/wander_controller_behavior.hpp"

class Asset;
class Input;

class gary_controller : public CustomAssetController {
public:
    explicit gary_controller(Asset* self);
    ~gary_controller() override = default;
    

protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    animation_update::custom_controllers::WanderControllerBehavior wander_behavior_;
};
