#pragma once
#include "assets/asset_controller.hpp"

class Asset;
class Input;

class DefaultController : public AssetController {

public:
    DefaultController(Asset* self);
    ~DefaultController() override = default;
    void update(const Input& in) override;
    void process_pending_attacks(Asset& self) override;

private:
    Asset* self_ = nullptr;
};

