#pragma once
#include "animation/controllers/custom_controllers/custom_asset_controller.hpp"

class Asset;
class Input;

class default_controller : public CustomAssetController {

public:
    default_controller(Asset* self);
    ~default_controller() override = default;
protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;
};
