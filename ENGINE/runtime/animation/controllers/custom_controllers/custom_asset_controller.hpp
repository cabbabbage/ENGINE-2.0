#pragma once

#include "assets/asset/asset_controller.hpp"

class Asset;
class Assets;
class Input;

// Base for engine custom controllers. Keeps a stable self pointer and routes
// engine callbacks into controller-specific hooks.
class CustomAssetController : public AssetController {
public:
    explicit CustomAssetController(Asset* self);
    ~CustomAssetController() override;

    void update(const Input& in) final;
    void process_pending_attacks(Asset& self) final;

protected:
    Asset* self_ptr() const { return self_; }
    Assets* assets() const;

    virtual void on_update(const Input& in) = 0;
    virtual void on_process_pending_attacks(Asset& self) = 0;

private:
    Asset* self_ = nullptr;
};
