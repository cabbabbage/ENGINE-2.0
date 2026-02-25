#ifndef ANCHOR_BOUND_ASSET_HELPER_HPP
#define ANCHOR_BOUND_ASSET_HELPER_HPP

#include <memory>
#include <string>
#include <unordered_map>

class Asset;
class Assets;

// A single, shared binding helper used by all custom controllers.
// Responsibilities:
//  - Create potential child assets during controller init (create_child).
//  - Keep children dormant (not attached to the grid) until explicitly bound.
//  - Bind/unbind children to controller anchors; optionally auto-unbind after N ticks.
//  - Reactivate/deactivate updates & rendering solely through the binding lifecycle.
//
// API surface kept minimal and uniform to discourage ad‑hoc controller logic.
class AnchorBoundAssetHelper {
public:
    explicit AnchorBoundAssetHelper(Asset* controller);
    ~AnchorBoundAssetHelper();

    // Public API available to controllers.
    bool bind(const std::string& child_asset_id, const std::string& anchor_name);
    bool bind_for_ticks(const std::string& child_asset_id, const std::string& anchor_name, int ticks);
    void unbind(const std::string& child_asset_id);

private:
    // Child creation kept internal to enforce API surface.
    Asset* create_child(const std::string& asset_id);
    void update();

    struct ChildState {
        std::unique_ptr<Asset> dormant; // Owning pointer when detached from grid.
        Asset* live = nullptr;          // Raw pointer when attached to grid.
        std::string anchor_name;
        int ticks_remaining = -1;       // -1 => infinite
        bool active = false;
    };

    ChildState* get_child_state(const std::string& id);
    void ensure_hidden(Asset* child);
    bool attach_child(const std::string& id, ChildState& state, const std::string& anchor_name);
    void detach_child(ChildState& state);

    Asset* controller_ = nullptr;
    Assets* assets_ = nullptr;
    std::unordered_map<std::string, ChildState> children_;

    friend class vibble_controller; // allow controller to pre-create children during init
    friend class Assets;            // allow engine to drive updates each frame
};

#endif
