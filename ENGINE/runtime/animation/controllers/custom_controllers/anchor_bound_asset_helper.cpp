#include "anchor_bound_asset_helper.hpp"

#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"
#include "utils/AnchorPointResolver.hpp"

#include <algorithm>

namespace {

// Toggle verbose vibble anchor diagnostics.
constexpr bool kVibbleAnchorDebug = true;

std::string controller_label(const Asset* controller_asset) {
    if (!controller_asset) {
        return "<null-controller>";
    }
    if (!controller_asset->spawn_id.empty()) {
        return controller_asset->spawn_id;
    }
    if (controller_asset->info) {
        return controller_asset->info->name;
    }
    return "<unnamed-controller>";
}

std::string asset_label(const Asset* asset) {
    if (!asset) return "<null-asset>";
    if (!asset->spawn_id.empty()) return asset->spawn_id;
    if (asset->info) return asset->info->name;
    return "<unnamed-asset>";
}

} // namespace

AnchorBoundAssetHelper::Handle AnchorBoundAssetHelper::create_asset_and_bind_to_anchor(
        const std::string& anchor_name,
        const std::string& asset_name_to_bind) {
    if (!controller_asset_) {
        vibble::log::warn("[AnchorBoundAssetHelper] Cannot create follower '" + asset_name_to_bind +
                          "': controller asset is null.");
        return {};
    }

    Assets* assets = controller_asset_->get_assets();
    if (!assets) {
        vibble::log::warn("[AnchorBoundAssetHelper] Cannot create follower '" + asset_name_to_bind +
                          "' for controller '" + controller_label(controller_asset_) +
                          "': Assets service unavailable.");
        return {};
    }

    if (kVibbleAnchorDebug) {
        try {
            if (auto resolved = controller_asset_->anchor_state(anchor_name, anchor_points::GridMaterialization::Ensure)) {
                const std::string status = resolved->missing ? "missing" : "resolved";
                vibble::log::info("[AnchorBoundAssetHelper] Probe anchor '" + anchor_name + "' on controller '" +
                                  controller_label(controller_asset_) + "': " + status +
                                  " world_px=(" + std::to_string(resolved->world_px.x) + "," + std::to_string(resolved->world_px.y) +
                                  ") z=" + std::to_string(resolved->world_z) +
                                  " layer=" + std::to_string(resolved->resolution_layer));
            }
        } catch (const std::exception& ex) {
            vibble::log::warn("[AnchorBoundAssetHelper] Anchor probe threw for controller '" +
                              controller_label(controller_asset_) + "' anchor '" + anchor_name + "': " + ex.what());
        }
    }

    Asset* created = assets->create_asset_and_bind_to_anchor(controller_asset_, anchor_name, asset_name_to_bind);
    if (!created) {
        vibble::log::error("[AnchorBoundAssetHelper] Failed to create/bind follower '" + asset_name_to_bind +
                           "' to anchor '" + anchor_name + "' for controller '" +
                           controller_label(controller_asset_) + "'.");
        return {};
    }

    if (kVibbleAnchorDebug) {
        vibble::log::info("[AnchorBoundAssetHelper] Bound follower '" + asset_name_to_bind +
                          "' to anchor '" + anchor_name + "' on controller '" +
                          controller_label(controller_asset_) + "'.");
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
        if (kVibbleAnchorDebug) {
            vibble::log::info("[AnchorBoundAssetHelper] Unbound and deleted follower asset '" +
                              asset_label(handle.asset) + "' from controller '" +
                              controller_label(controller_asset_) + "'.");
        }
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
        if (kVibbleAnchorDebug) {
            vibble::log::debug("[AnchorBoundAssetHelper] Cleaned up follower asset '" +
                               asset_label(asset) + "' during controller teardown for '" +
                               controller_label(controller_asset_) + "'.");
        }
    }
    created_assets_.clear();
}
