#include "anchor_bound_asset_helper.hpp"

#include "assets/asset/Asset.hpp"
#include "animation/controllers/shared/child_asset.hpp"

#include <vector>

namespace anchor_bound_asset_helper {

AnchorBoundAssetHelper& AnchorBoundAssetHelper::instance() {
    static AnchorBoundAssetHelper helper;
    return helper;
}

void AnchorBoundAssetHelper::register_child(Asset* owner,
                                            ChildAsset* child,
                                            Asset* child_asset,
                                            const std::string& anchor_name) {
    if (!owner || !child || !child_asset || anchor_name.empty()) {
        return;
    }
    bindings_[child_asset] = BindingRecord{owner, child, child_asset, anchor_name};
}

void AnchorBoundAssetHelper::unregister_child(Asset* child_asset) {
    if (!child_asset) {
        return;
    }
    bindings_.erase(child_asset);
}

bool AnchorBoundAssetHelper::is_child_bound(const Asset* child_asset) const {
    if (!child_asset) {
        return false;
    }
    return bindings_.find(const_cast<Asset*>(child_asset)) != bindings_.end();
}

void AnchorBoundAssetHelper::notify_anchor_changed(Asset* owner, const std::string& anchor_name) {
    if (!owner) {
        return;
    }
    std::vector<ChildAsset*> to_update;
    to_update.reserve(bindings_.size());
    for (const auto& [child_asset, record] : bindings_) {
        if (record.owner != owner) {
            continue;
        }
        if (!anchor_name.empty() && record.anchor_name != anchor_name) {
            continue;
        }
        if (record.child) {
            to_update.push_back(record.child);
        }
    }
    for (ChildAsset* child : to_update) {
        child->update();
    }
}

} // namespace anchor_bound_asset_helper
