#pragma once
#include "animation/controllers/custom_controller.hpp"

class Asset;
class Input;

class gary_controller : public custom_controller_api::CustomControllerBase {
public:
    explicit gary_controller(Asset* self);
    ~gary_controller() override = default;
    

protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;

};
