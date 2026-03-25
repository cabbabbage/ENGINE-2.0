#ifndef FROG_CONTROLLER_HPP
#define FROG_CONTROLLER_HPP

#include "animation/controllers/custom_controllers/custom_asset_controller.hpp"

#include <SDL3/SDL.h>

class Asset;
class Input;

class frog_controller : public CustomAssetController {

public:

    explicit frog_controller(Asset* self);

    ~frog_controller() override = default;

protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;
};

#endif
