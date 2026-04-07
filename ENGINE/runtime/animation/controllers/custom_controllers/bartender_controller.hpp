#pragma once
#include "animation/controllers/shared/custom_asset_controller.hpp"
#include "animation/controllers/shared/wander_controller_behavior.hpp"

class Asset;
class Input;

class bartender_controller : public CustomAssetController {
public:
    explicit bartender_controller(Asset* self);
    ~bartender_controller() override = default;


protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    animation_update::custom_controllers::WanderControllerBehavior wander_behavior_;
};
