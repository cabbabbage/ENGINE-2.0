#ifndef ANCHOR_BOUND_ASSET_HELPER_HPP
#define ANCHOR_BOUND_ASSET_HELPER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/asset/anchor_point.hpp"

class Asset;
class Assets;

// Runtime-owned anchor binding utility exposed to controllers through a stable API.
// Ownership model: callers own child assets; helper only tracks, hides, and detaches them.
// Parent/child removals are detected via Assets and purge tracked bindings automatically.
class AnchorBoundAssetHelper {
public:
    explicit AnchorBoundAssetHelper(Asset* controller);
    AnchorBoundAssetHelper(Assets* assets_owner, Asset* controller = nullptr);
    ~AnchorBoundAssetHelper();

    // API used by custom controllers (stable).
    bool tick_for_frame();
    bool bind_child_to_anchor_names(Asset& parent,
                                    Asset& child,
                                    const std::string& parent_anchor_name,
                                    const std::string& child_anchor_name,
                                    std::optional<std::string> depth_policy = std::nullopt,
                                    std::optional<std::string> layer_policy = std::nullopt,
                                    int ticks = -1);
    bool bind_child_to_anchor(Asset& parent,
                              Asset& child,
                              const AnchorPoint& anchor,
                              const std::string& child_anchor_name = std::string{});
    bool bind_child_for_ticks(Asset& parent,
                              Asset& child,
                              const AnchorPoint& anchor,
                              int ticks = -1,
                              const std::string& child_anchor_name = std::string{});
    void unbind_child(Asset& parent, Asset& child);
    void unbind_child(Asset& child);
    void purge_bindings_for_asset(const Asset* asset);

private:
    enum class DepthPolicy {
        AnchorDerived,
        MatchOwner
    };

    enum class LayerPolicy {
        AnchorDerived,
        MatchControllerAsset
    };

    struct BindingRecord {
        std::string parent_id;
        std::string child_id;
        Asset* parent = nullptr;
        Asset* child = nullptr;
        std::uintptr_t parent_instance_key = 0;
        std::uintptr_t child_instance_key = 0;
        std::uint64_t bind_sequence = 0;
        std::string anchor_name;
        std::optional<std::size_t> anchor_index{};
        std::optional<std::uint64_t> expiry_tick{};
        int ticks_remaining = -1; // -1 => infinite
        bool bound = false;
        bool currently_active = false;
        bool registered_with_parent = false;
        int last_anchor_depth_offset = 0;
        std::string child_anchor_name;
        DepthPolicy depth_policy = DepthPolicy::AnchorDerived;
        LayerPolicy layer_policy = LayerPolicy::AnchorDerived;
    };

    static DepthPolicy parse_depth_policy(const std::optional<std::string>& raw);
    static LayerPolicy parse_layer_policy(const std::optional<std::string>& raw);

    bool resolve_binding_entities(BindingRecord& record);
    std::uintptr_t binding_key_for_child(const Asset& child) const;
    std::string asset_stable_id(const Asset* asset) const;
    std::optional<AnchorPoint> resolve_anchor(BindingRecord& record) const;
    std::optional<AnchorPoint> resolve_child_anchor(BindingRecord& record) const;
    void teardown_binding(BindingRecord& state);
    std::vector<std::uintptr_t> build_binding_order(bool& has_cycle,
                                                    std::size_t& cycle_nodes) const;

    bool update();
    bool apply_binding_tick(BindingRecord& state);

    bool set_child_hidden_state(Asset* child, bool hidden) const;
    void log_binding_tick(const BindingRecord& state,
                          bool anchor_available,
                          int anchor_world_x,
                          int anchor_world_y,
                          int child_world_x,
                          int child_world_y) const;
    void log_cycle_warning(std::size_t cycle_nodes) const;
    void log_alignment_mismatch(const BindingRecord& state,
                                int expected_world_x,
                                int expected_world_y,
                                int expected_world_z,
                                int expected_resolution_layer) const;

    Asset* controller_ = nullptr;
    Assets* assets_ = nullptr;
    std::unordered_map<std::uintptr_t, BindingRecord> children_;
    std::uint64_t tick_counter_ = 0;
    std::uint64_t bind_sequence_counter_ = 1;
    std::uint64_t last_cycle_warning_tick_ = 0;
    bool debug_tick_logging_enabled_ = false;

    friend class vibble_controller;
    friend class Assets;
};

#endif
