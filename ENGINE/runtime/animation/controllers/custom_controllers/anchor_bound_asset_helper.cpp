#include "anchor_bound_asset_helper.hpp"

#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"

#include <algorithm>

AnchorBoundAssetHelper::Handle AnchorBoundAssetHelper::create_asset_and_bind_to_anchor(
        const std::string& anchor_name,
        const std::string& asset_name_to_bind) {
    if (!controller_asset_) {
        return {};
    }

    Assets* assets = controller_asset_->get_assets();
    if (!assets) {
        return {};
    }

    Asset* created = assets->create_asset_and_bind_to_anchor(controller_asset_, anchor_name, asset_name_to_bind);
    if (!created) {
        return {};
    }

    created_assets_.push_back(created);
    return Handle{created};
}

bool AnchorBoundAssetHelper::unbind_and_delete_created(const Handle& handle) {
    if (!handle.asset || !controller_asset_) {
        return false;
    }

    Assets* assets = controller_asset_->get_assets();
    if (!assets) {
        return false;
    }

    const bool removed = assets->unbind_and_delete_created(controller_asset_, handle.asset);
    if (removed) {
        created_assets_.erase(std::remove(created_assets_.begin(), created_assets_.end(), handle.asset), created_assets_.end());
    }
    return removed;
}

void AnchorBoundAssetHelper::cleanup_all() {
    if (!controller_asset_) {
        created_assets_.clear();
        return;
    }

    Assets* assets = controller_asset_->get_assets();
    if (!assets) {
        created_assets_.clear();
        return;
    }

    for (Asset* asset : created_assets_) {
        (void)assets->unbind_and_delete_created(controller_asset_, asset);
    }
    created_assets_.clear();
}
