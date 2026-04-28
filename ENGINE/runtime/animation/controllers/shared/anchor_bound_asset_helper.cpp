#include "anchor_bound_asset_helper.hpp"

#include "animation/controllers/shared/anchor_binding_order.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation_frame.hpp"
#include "animation/controllers/shared/child_asset.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace anchor_bound_asset_helper {
namespace {

int child_frame_index(const Asset* child_asset) {
    if (!child_asset) {
        return -1;
    }
    const AnimationFrame* frame = child_asset->current_animation_frame();
    return frame ? frame->frame_index : -1;
}

void hash_append_bytes(std::uint64_t& hash, const void* data, std::size_t size) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= kFnvPrime;
    }
}

void hash_append_string(std::uint64_t& hash, std::string_view value) {
    hash_append_bytes(hash, value.data(), value.size());
    const unsigned char separator = 0xFFu;
    hash_append_bytes(hash, &separator, sizeof(separator));
}

void hash_append_int(std::uint64_t& hash, int value) {
    hash_append_bytes(hash, &value, sizeof(value));
}

std::uint64_t deterministic_binding_sort_key(const Asset* owner,
                                             const Asset* child_asset,
                                             std::string_view anchor_name) {
    std::uint64_t hash = 1469598103934665603ull;
    hash_append_string(hash, anchor_name);

    if (owner) {
        hash_append_string(hash, owner->spawn_id);
        if (owner->info) {
            hash_append_string(hash, owner->info->name);
        }
        hash_append_int(hash, owner->world_x());
        hash_append_int(hash, owner->world_y());
        hash_append_int(hash, owner->world_z());
        hash_append_int(hash, owner->grid_resolution);
    }

    if (child_asset) {
        hash_append_string(hash, child_asset->spawn_id);
        if (child_asset->info) {
            hash_append_string(hash, child_asset->info->name);
        }
        hash_append_int(hash, child_asset->world_x());
        hash_append_int(hash, child_asset->world_y());
        hash_append_int(hash, child_asset->world_z());
        hash_append_int(hash, child_asset->grid_resolution);
    }

    return hash;
}

} // namespace

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
    bindings_[child_asset] = BindingRecord{
        owner,
        child,
        child_asset,
        anchor_name,
        child_frame_index(child_asset)};
    ++bindings_version_;
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
    ++bindings_version_;
}

bool AnchorBoundAssetHelper::is_child_bound(const Asset* child_asset) const {
    if (!child_asset) {
        return false;
    }
    return bindings_.find(const_cast<Asset*>(child_asset)) != bindings_.end();
}

std::vector<AnchorBoundAssetHelper::DebugBinding> AnchorBoundAssetHelper::debug_bindings_snapshot() const {
    std::vector<DebugBinding> snapshot;
    snapshot.reserve(bindings_.size());
    for (const auto& [child_asset, record] : bindings_) {
        (void)child_asset;
        if (!record.owner || !record.child_asset || record.anchor_name.empty()) {
            continue;
        }
        snapshot.push_back(DebugBinding{record.owner, record.child_asset, record.anchor_name});
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const DebugBinding& lhs, const DebugBinding& rhs) {
        if (lhs.owner != rhs.owner) {
            return lhs.owner < rhs.owner;
        }
        if (lhs.child_asset != rhs.child_asset) {
            return lhs.child_asset < rhs.child_asset;
        }
        return lhs.anchor_name < rhs.anchor_name;
    });
    return snapshot;
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

AnchorBoundAssetHelper::FlushResult AnchorBoundAssetHelper::flush_pending_updates_detailed() {
    FlushResult result{};
    if (flush_in_progress_) {
        return result;
    }

    for (auto& [child_asset, record] : bindings_) {
        (void)child_asset;
        if (!record.child) {
            continue;
        }
        const int current_frame_index = child_frame_index(record.child_asset);
        if (current_frame_index != record.last_child_frame_index) {
            pending_children_.insert(record.child);
            record.last_child_frame_index = current_frame_index;
        }
    }

    if (pending_children_.empty()) {
        return result;
    }

    struct FlushGuard {
        bool& flag;
        ~FlushGuard() { flag = false; }
    };

    flush_in_progress_ = true;
    FlushGuard guard{flush_in_progress_};

    struct BindingSnapshot {
        Asset* owner = nullptr;
        Asset* child_asset = nullptr;
        std::uint64_t sort_key = 0;
    };

    std::unordered_map<ChildAsset*, BindingSnapshot> binding_by_child;
    std::unordered_map<Asset*, ChildAsset*> child_by_asset;
    std::uint64_t cached_binding_version = 0;
    auto rebuild_binding_maps_if_needed = [&]() {
        if (cached_binding_version == bindings_version_) {
            return;
        }

        binding_by_child.clear();
        child_by_asset.clear();
        binding_by_child.reserve(bindings_.size() * 2);
        child_by_asset.reserve(bindings_.size() * 2);
        for (const auto& [child_asset, record] : bindings_) {
            (void)child_asset;
            if (!record.child) {
                continue;
            }

            binding_by_child[record.child] = BindingSnapshot{
                record.owner,
                record.child_asset,
                deterministic_binding_sort_key(record.owner,
                                               record.child_asset,
                                               record.anchor_name)};
            if (record.child_asset) {
                child_by_asset[record.child_asset] = record.child;
            }
        }
        cached_binding_version = bindings_version_;
    };

    std::vector<ChildAsset*> wave;
    std::unordered_set<ChildAsset*> wave_lookup;
    std::vector<anchor_binding_order::Node> nodes;
    std::unordered_map<std::uintptr_t, ChildAsset*> child_by_id;

    while (!pending_children_.empty()) {
        ++result.wave_count;

        rebuild_binding_maps_if_needed();

        wave.clear();
        wave.reserve(pending_children_.size());
        for (ChildAsset* child : pending_children_) {
            if (child) {
                wave.push_back(child);
            }
        }
        pending_children_.clear();
        result.children_considered += wave.size();
        if (wave.empty()) {
            continue;
        }

        wave_lookup.clear();
        wave_lookup.reserve(wave.size() * 2);
        for (ChildAsset* child : wave) {
            if (child) {
                wave_lookup.insert(child);
            }
        }

        nodes.clear();
        nodes.reserve(wave.size());
        child_by_id.clear();
        child_by_id.reserve(wave.size() * 2);
        for (ChildAsset* child : wave) {
            if (!child) {
                continue;
            }
            const auto binding_it = binding_by_child.find(child);
            if (binding_it == binding_by_child.end()) {
                continue;
            }
            const BindingSnapshot& binding = binding_it->second;
            std::optional<std::uintptr_t> depends_on{};
            if (binding.owner) {
                const auto owner_child_it = child_by_asset.find(binding.owner);
                if (owner_child_it != child_by_asset.end() &&
                    owner_child_it->second &&
                    owner_child_it->second != child &&
                    wave_lookup.count(owner_child_it->second) > 0) {
                    depends_on = reinterpret_cast<std::uintptr_t>(owner_child_it->second);
                }
            }

            const std::uintptr_t id = reinterpret_cast<std::uintptr_t>(child);
            nodes.push_back(anchor_binding_order::Node{
                id,
                depends_on,
                binding.sort_key});
            child_by_id[id] = child;
        }

        const anchor_binding_order::Result order = anchor_binding_order::compute(nodes);
        for (std::uintptr_t id : order.ordered_ids) {
            const auto child_it = child_by_id.find(id);
            if (child_it == child_by_id.end() || !child_it->second) {
                continue;
            }
            const ChildAsset::UpdateResult child_result = child_it->second->update_detailed();
            result.any_change = result.any_change || child_result.any_change;
            result.needs_repass = result.needs_repass || child_result.needs_repass;
            result.needs_traversal_refresh =
                result.needs_traversal_refresh || child_result.needs_traversal_refresh;
            if (child_result.any_change) {
                ++result.children_updated;
            }
        }
    }

    for (auto& [child_asset, record] : bindings_) {
        (void)child_asset;
        record.last_child_frame_index = child_frame_index(record.child_asset);
    }

    return result;
}

bool AnchorBoundAssetHelper::flush_pending_updates() {
    return flush_pending_updates_detailed().any_change;
}

} // namespace anchor_bound_asset_helper
