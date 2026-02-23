#ifndef ANCHOR_BOUND_ASSET_HELPER_HPP
#define ANCHOR_BOUND_ASSET_HELPER_HPP

#include <string>
#include <vector>

class Asset;
class Assets;

class AnchorBoundAssetHelper {
public:
    struct Handle {
        Asset* asset = nullptr;
        bool valid() const { return asset != nullptr; }
    };

    explicit AnchorBoundAssetHelper(Asset* controller_asset)
        : controller_asset_(controller_asset) {}

    Handle create_asset_and_bind_to_anchor(const std::string& anchor_name,
                                           const std::string& asset_name_to_bind);
    bool unbind_and_delete_created(const Handle& handle);
    void cleanup_all();

private:
    Asset* controller_asset_ = nullptr;
    std::vector<Asset*> created_assets_;
};

#endif
