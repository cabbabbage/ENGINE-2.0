#ifndef spider_CONTROLLER_HPP
#define spider_CONTROLLER_HPP

#include "assets/asset/asset_controller.hpp"

class Asset;
class Assets;
class Input;

class spider_controller : public AssetController {

public:
    spider_controller(Assets* assets, Asset* self);
    ~spider_controller() override = default;
    void update(const Input&) override;
    void process_pending_attacks(Asset& self) override;

private:
    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;
};

#endif
