#ifndef BOMB_CONTROLLER_HPP
#define BOMB_CONTROLLER_HPP

#include "animation/controllers/shared/custom_asset_controller.hpp"

class Asset;
class Input;

class bomb_controller : public CustomAssetController {

public:
    explicit bomb_controller(Asset* self);
    ~bomb_controller() override = default;

protected:
    void on_update(const Input&) override;
    void on_process_pending_attacks(Asset& self) override;
};

#endif
