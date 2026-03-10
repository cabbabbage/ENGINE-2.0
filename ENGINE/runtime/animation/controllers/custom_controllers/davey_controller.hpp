#ifndef DAVEY_CONTROLLER_HPP
#define DAVEY_CONTROLLER_HPP

#include "assets/asset/asset_controller.hpp"

class Assets;
class Asset;
class Input;

class davey_controller : public AssetController {

public:
    davey_controller(Assets* assets, Asset* self);
    ~davey_controller() = default;
    void update(const Input& in) override;
    void process_pending_attacks(Asset& self) override;

private:
    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;
};

#endif
