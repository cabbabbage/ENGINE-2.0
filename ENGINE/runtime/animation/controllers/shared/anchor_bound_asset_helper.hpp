#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

class Asset;
class ChildAsset;

namespace anchor_bound_asset_helper {

class AnchorBoundAssetHelper {
public:
    struct DebugBinding {
        Asset* owner = nullptr;
        Asset* child_asset = nullptr;
        std::string anchor_name;
    };

    struct FlushResult {
        bool any_change = false;
        bool needs_repass = false;
        bool needs_traversal_refresh = false;
        std::size_t wave_count = 0;
        std::size_t children_considered = 0;
        std::size_t children_updated = 0;
    };

    static AnchorBoundAssetHelper& instance();

    void register_child(Asset* owner, ChildAsset* child, Asset* child_asset, const std::string& anchor_name);
    void unregister_child(Asset* child_asset);
    bool is_child_bound(const Asset* child_asset) const;
    std::vector<DebugBinding> debug_bindings_snapshot() const;
    void notify_anchor_changed(Asset* owner, const std::string& anchor_name);
    FlushResult flush_pending_updates_detailed();
    bool flush_pending_updates();

private:
    struct BindingRecord {
        Asset* owner = nullptr;
        ChildAsset* child = nullptr;
        Asset* child_asset = nullptr;
        std::string anchor_name;
        int last_child_frame_index = -1;
    };

    std::unordered_map<Asset*, BindingRecord> bindings_;
    std::unordered_set<ChildAsset*> pending_children_;
    bool flush_in_progress_ = false;
    std::uint64_t bindings_version_ = 1;
};

} // namespace anchor_bound_asset_helper
