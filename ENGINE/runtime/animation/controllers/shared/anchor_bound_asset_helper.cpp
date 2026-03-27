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
    auto it = bindings_.find(child_asset);
    if (it != bindings_.end() && it->second.child) {
        pending_children_.erase(it->second.child);
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
    for (const auto& [child_asset, record] : bindings_) {
        (void)child_asset;
        if (record.owner != owner) {
            continue;
        }
        if (!anchor_name.empty() && record.anchor_name != anchor_name) {
            continue;
        }
        if (record.child) {
            pending_children_.insert(record.child);
        }
    }
}

void AnchorBoundAssetHelper::flush_pending_updates() {
    if (flush_in_progress_ || pending_children_.empty()) {
        return;
    }

    flush_in_progress_ = true;
    // Child updates can trigger additional anchor dirties; drain until stable.
    while (!pending_children_.empty()) {
        std::vector<ChildAsset*> to_update;
        to_update.reserve(pending_children_.size());
        for (ChildAsset* child : pending_children_) {
            if (child) {
                to_update.push_back(child);
            }
        }
        pending_children_.clear();

        for (ChildAsset* child : to_update) {
            child->update();
        }
    }
    flush_in_progress_ = false;
}

} // namespace anchor_bound_asset_helper
