#pragma once

#include "assets/asset/asset_controller.hpp"

class Asset;
class Assets;
class Input;

// Base for engine custom controllers. It installs the active controller context
// while controller callbacks run so ChildAsset can resolve the owning asset
// implicitly from controller code.
class CustomAssetController : public AssetController {
public:
    explicit CustomAssetController(Asset* self);
    ~CustomAssetController() override;

    void update(const Input& in) final;
    void process_pending_attacks(Asset& self) final;

    static Asset* active_owner_asset();
    static Assets* active_assets();

protected:
    Asset* self_ptr() const { return self_; }
    Assets* assets() const;

    virtual void on_update(const Input& in) = 0;
    virtual void on_process_pending_attacks(Asset& self) = 0;

private:
    class ScopedContext {
    public:
        explicit ScopedContext(CustomAssetController& controller);
        ~ScopedContext();

    private:
        Asset* previous_owner_ = nullptr;
        Assets* previous_assets_ = nullptr;
    };

    static thread_local Asset* tls_active_owner_;
    static thread_local Assets* tls_active_assets_;

    Asset* self_ = nullptr;
};
