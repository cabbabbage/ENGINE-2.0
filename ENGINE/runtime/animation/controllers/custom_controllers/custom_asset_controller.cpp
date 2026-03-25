#include "custom_asset_controller.hpp"

#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

thread_local Asset* CustomAssetController::tls_active_owner_ = nullptr;
thread_local Assets* CustomAssetController::tls_active_assets_ = nullptr;

CustomAssetController::CustomAssetController(Asset* self)
    : self_(self) {}

CustomAssetController::~CustomAssetController() = default;

void CustomAssetController::update(const Input& in) {
    ScopedContext scope(*this);
    on_update(in);
}

void CustomAssetController::process_pending_attacks(Asset& self) {
    ScopedContext scope(*this);
    on_process_pending_attacks(self);
}

Asset* CustomAssetController::active_owner_asset() {
    return tls_active_owner_;
}

Assets* CustomAssetController::active_assets() {
    return tls_active_assets_;
}

Assets* CustomAssetController::assets() const {
    return self_ ? self_->get_assets() : nullptr;
}

CustomAssetController::ScopedContext::ScopedContext(CustomAssetController& controller)
    : previous_owner_(tls_active_owner_)
    , previous_assets_(tls_active_assets_) {
    tls_active_owner_ = controller.self_;
    tls_active_assets_ = controller.self_ ? controller.self_->get_assets() : nullptr;
}

CustomAssetController::ScopedContext::~ScopedContext() {
    tls_active_owner_ = previous_owner_;
    tls_active_assets_ = previous_assets_;
}
