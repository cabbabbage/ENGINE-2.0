#include "custom_asset_controller.hpp"

#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

CustomAssetController::CustomAssetController(Asset* self)
    : self_(self) {}

CustomAssetController::~CustomAssetController() = default;

void CustomAssetController::update(const Input& in) {
    on_update(in);
}

void CustomAssetController::process_pending_attacks(Asset& self) {
    on_process_pending_attacks(self);
}

Assets* CustomAssetController::assets() const {
    return self_ ? self_->get_assets() : nullptr;
}
