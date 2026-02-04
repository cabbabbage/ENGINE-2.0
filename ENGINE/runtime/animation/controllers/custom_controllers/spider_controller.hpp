#ifndef spider_CONTROLLER_HPP
#define spider_CONTROLLER_HPP

#include "assets/asset_controller.hpp"

class Asset;
class Assets;
class Input;

class spiderController : public AssetController {

public:
    spiderController(Assets* assets, Asset* self);
    ~spiderController() override = default;
    void update(const Input&) override;
    void process_pending_attacks(Asset& self) override;

private:
    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;
};

#endif
