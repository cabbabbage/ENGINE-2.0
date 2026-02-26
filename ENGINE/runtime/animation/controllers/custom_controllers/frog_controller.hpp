#ifndef FROG_CONTROLLER_HPP
#define FROG_CONTROLLER_HPP

#include "assets/asset_controller.hpp"

#include <SDL3/SDL.h>

class Assets;
class Asset;
class Input;

class frog_controller : public AssetController {

public:

    frog_controller(Assets* assets, Asset* self);

    ~frog_controller() override = default;
    void update(const Input& in) override;
    void process_pending_attacks(Asset& self) override;

private:
    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;
};

#endif
