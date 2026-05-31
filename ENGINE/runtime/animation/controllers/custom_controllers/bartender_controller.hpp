#pragma once
#include "animation/controllers/custom_controller.hpp"

class Asset;
class Input;

class bartender_controller : public custom_controller_api::CustomControllerBase {
public:
    explicit bartender_controller(Asset* self);
    ~bartender_controller() override = default;


protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;

};
