#ifndef ANCHOR_BOUND_ASSET_HELPER_HPP
#define ANCHOR_BOUND_ASSET_HELPER_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

class Asset;
class Assets;

// Runtime-owned anchor binding utility exposed to controllers through a stable API.
class AnchorBoundAssetHelper {
public:
    explicit AnchorBoundAssetHelper(Asset* controller);
    ~AnchorBoundAssetHelper();

    // API used by custom controllers (kept stable).
    void tick_for_frame();
    bool bind(const std::string& child_asset_id, const std::string& anchor_name);
    bool bind_for_ticks(const std::string& child_asset_id, const std::string& anchor_name, int ticks);
    void unbind(const std::string& child_asset_id);

private:
    struct ChildState {
        Asset* child = nullptr;
        std::string anchor_name;
        int ticks_remaining = -1; // -1 => infinite
        bool bound = false;
        bool currently_active = false;
    };

    Asset* create_child(const std::string& asset_id);
    ChildState* get_child_state(const std::string& id);

    void update();
    void apply_binding_tick(const std::string& child_id, ChildState& state);

    void set_child_hidden_state(Asset* child, bool hidden) const;
    void log_binding_tick(const std::string& child_id,
                          const ChildState& state,
                          bool anchor_available,
                          int anchor_world_x,
                          int anchor_world_y,
                          int child_world_x,
                          int child_world_y) const;

    Asset* controller_ = nullptr;
    Assets* assets_ = nullptr;
    std::unordered_map<std::string, ChildState> children_;
    bool debug_tick_logging_enabled_ = false;

    friend class vibble_controller;
    friend class Assets;
};

#endif
