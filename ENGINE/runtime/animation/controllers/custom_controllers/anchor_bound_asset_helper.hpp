#pragma once

#include <unordered_map>
#include <string>

class Asset;
class ChildAsset;

namespace anchor_bound_asset_helper {

class AnchorBoundAssetHelper {
public:
    static AnchorBoundAssetHelper& instance();

    void register_child(Asset* owner, ChildAsset* child, Asset* child_asset, const std::string& anchor_name);
    void unregister_child(Asset* child_asset);
    bool is_child_bound(const Asset* child_asset) const;
    void notify_anchor_changed(Asset* owner, const std::string& anchor_name);

private:
    struct BindingRecord {
        Asset* owner = nullptr;
        ChildAsset* child = nullptr;
        Asset* child_asset = nullptr;
        std::string anchor_name;
    };

    std::unordered_map<Asset*, BindingRecord> bindings_;
};

} // namespace anchor_bound_asset_helper
