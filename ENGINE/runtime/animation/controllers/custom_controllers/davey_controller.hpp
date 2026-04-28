#ifndef DAVEY_CONTROLLER_HPP
#define DAVEY_CONTROLLER_HPP

#include "animation/controllers/shared/custom_controller_api.hpp"

class Asset;
class Input;

class davey_controller : public CustomAssetController {

public:
    explicit davey_controller(Asset* self);
    ~davey_controller() = default;

protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;
};

#endif
