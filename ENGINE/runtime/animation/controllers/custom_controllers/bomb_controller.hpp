#ifndef BOMB_CONTROLLER_HPP
#define BOMB_CONTROLLER_HPP

#include "assets/asset_controller.hpp"

class Asset;
class Assets;
class Input;

class bomb_controller : public AssetController {

public:
    bomb_controller(Assets* assets, Asset* self);
    ~bomb_controller() override = default;
    void update(const Input&) override;
    void process_pending_attacks(Asset& self) override;

private:
    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;
};

#endif
