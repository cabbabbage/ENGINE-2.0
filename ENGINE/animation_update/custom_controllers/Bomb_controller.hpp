#ifndef BOMB_CONTROLLER_HPP
#define BOMB_CONTROLLER_HPP

#include "asset/asset_controller.hpp"

class Asset;
class Assets;
class Input;

class BombController : public AssetController {

public:
    BombController(Assets* assets, Asset* self);
    ~BombController() override = default;
    void update(const Input&) override;
    void process_pending_attacks(Asset& self) override;

private:
    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;
};

#endif
