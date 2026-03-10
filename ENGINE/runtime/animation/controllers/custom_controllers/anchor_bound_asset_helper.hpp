#ifndef ANCHOR_BOUND_ASSET_HELPER_HPP
#define ANCHOR_BOUND_ASSET_HELPER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "assets/asset/anchor_point.hpp"

class Asset;
class Assets;

// Runtime-owned anchor binding utility exposed to controllers through a stable API.
// Ownership model: callers own child assets; helper only tracks, hides, and detaches them.
// Parent/child removals are detected via Assets and purge tracked bindings automatically.
class AnchorBoundAssetHelper {
public:
    explicit AnchorBoundAssetHelper(Asset* controller);
    ~AnchorBoundAssetHelper();

    // API used by custom controllers (stable).
    void tick_for_frame();
    bool bind_child_to_anchor(Asset& parent, Asset& child, const AnchorPoint& anchor);
    bool bind_child_for_ticks(Asset& parent, Asset& child, const AnchorPoint& anchor, int ticks = -1);
    void unbind_child(Asset& parent, Asset& child);
    void purge_bindings_for_asset(const Asset* asset);

private:
    struct BindingRecord {
        std::string parent_id;
        std::string child_id;
        Asset* parent = nullptr;
        Asset* child = nullptr;
        std::string anchor_name;
        std::optional<std::size_t> anchor_index{};
        std::optional<std::uint64_t> expiry_tick{};
        int ticks_remaining = -1; // -1 => infinite
        bool bound = false;
        bool currently_active = false;
        bool registered_with_parent = false;
        int last_anchor_depth_offset = 0;
    };

    bool resolve_binding_entities(BindingRecord& record);
    std::string binding_key_for_child(const Asset& child) const;
    std::string asset_stable_id(const Asset* asset) const;
    std::optional<AnchorPoint> resolve_anchor(BindingRecord& record) const;
    void teardown_binding(BindingRecord& state);

    void update();
    void apply_binding_tick(const std::string& child_id, BindingRecord& state);

    void set_child_hidden_state(Asset* child, bool hidden) const;
    void log_binding_tick(const std::string& child_id,
                          const BindingRecord& state,
                          bool anchor_available,
                          int anchor_world_x,
                          int anchor_world_y,
                          int child_world_x,
                          int child_world_y) const;

    Asset* controller_ = nullptr;
    Assets* assets_ = nullptr;
    std::unordered_map<std::string, BindingRecord> children_;
    std::uint64_t tick_counter_ = 0;
    bool debug_tick_logging_enabled_ = false;

    friend class vibble_controller;
    friend class Assets;
};

#endif
