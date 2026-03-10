#pragma once
#include "assets/asset/asset_controller.hpp"

class Asset;
class Input;

class default_controller : public AssetController {

public:
    default_controller(Asset* self);
    ~default_controller() override = default;
    void update(const Input& in) override;
    void process_pending_attacks(Asset& self) override;

private:
    Asset* self_ = nullptr;
};
