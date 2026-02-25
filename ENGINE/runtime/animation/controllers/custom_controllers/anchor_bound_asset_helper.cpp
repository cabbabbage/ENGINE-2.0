#include "anchor_bound_asset_helper.hpp"

#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"

#include <algorithm>

AnchorBoundAssetHelper::Handle AnchorBoundAssetHelper::bind_follower(
        const std::string& anchor_name,
        const std::string& follower_asset_id,
        const std::string& follower_anchor_name) {
    if (!controller_asset_) {
        vibble::log::warn("[AnchorHelper] bind_follower aborted: controller asset is null (follower='" + follower_asset_id + "').");
        return {};
    }

    Assets* assets = controller_asset_->get_assets();
    if (!assets) {
        vibble::log::warn("[AnchorHelper] bind_follower aborted: controller asset '" +
                          (controller_asset_->info ? controller_asset_->info->name : std::string{"<unnamed>"}) +
                          "' has no owning Assets.");
        return {};
    }

    Asset::AnchorFollowTarget binding{};
    binding.anchor_name = anchor_name;
    if (!follower_anchor_name.empty()) {
        binding.follower_anchor_name = follower_anchor_name;
    }

    Asset* created = assets->bind_follower(controller_asset_, follower_asset_id, binding);
    if (!created) {
        vibble::log::warn("[AnchorHelper] bind_follower failed: controller='" +
                          (controller_asset_->info ? controller_asset_->info->name : std::string{"<unnamed>"}) +
                          "' follower_asset_id='" + follower_asset_id + "' anchor='" + anchor_name + "'.");
        return {};
    }

    created_assets_.push_back(created);
    vibble::log::debug("[AnchorHelper] bound follower '" + follower_asset_id + "' to controller '" +
                       (controller_asset_->info ? controller_asset_->info->name : std::string{"<unnamed>"}) +
                       "' at anchor '" + anchor_name +
                       (follower_anchor_name.empty() ? std::string{} : ("' (follower anchor '" + follower_anchor_name + "' )")) +
                       "'.");
    return Handle{created};
}

bool AnchorBoundAssetHelper::unbind_and_delete(const Handle& handle) {
    if (!handle.asset || !controller_asset_) {
        return false;
    }

    Assets* assets = controller_asset_->get_assets();
    if (!assets) {
        return false;
    }

    const bool removed = assets->unbind_follower(controller_asset_, handle.asset);
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
        (void)assets->unbind_follower(controller_asset_, asset);
    }
    created_assets_.clear();
}
