#ifndef DAVEY_CONTROLLER_HPP
#define DAVEY_CONTROLLER_HPP

#include "assets/asset_controller.hpp"

class Assets;
class Asset;
class Input;

class DaveyController : public AssetController {

public:
    DaveyController(Assets* assets, Asset* self);
    ~DaveyController() = default;
    void update(const Input& in) override;
    void process_pending_attacks(Asset& self) override;

private:
    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;
};

#endif

