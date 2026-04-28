#pragma once

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"

struct CollisionQueryContext {
    using CollisionEntryRef = const Assets::FrameCollisionEntry*;

    static constexpr int kCheckpointPaddingPx = 32;

    std::vector<CollisionEntryRef> entries{};
    bool loaded = false;
    int furthest_checkpoint_distance_px = 0;
    std::optional<std::string> engagement_target_asset_id = std::nullopt;

    void set_furthest_checkpoint_distance_px(int extent_px) {
        furthest_checkpoint_distance_px = std::max(0, extent_px);
        loaded = false;
        entries.clear();
    }

    static int resolve_search_radius(int neighbor_radius,
                                     int furthest_checkpoint_distance_px,
                                     int max_radius_cap) {
        const int neighbor = std::max(0, neighbor_radius);
        const int checkpoint_extent = std::max(0, furthest_checkpoint_distance_px);
        const int checkpoint_radius = checkpoint_extent + kCheckpointPaddingPx;
        const int requested = std::max(neighbor, checkpoint_radius);
        if (max_radius_cap > 0) {
            return std::min(requested, max_radius_cap);
        }
        return requested;
    }

    const std::vector<CollisionEntryRef>& collisions_for(const Asset& self) {
        if (!loaded) {
            loaded = true;
            const Assets* assets = self.get_assets();
            if (assets) {
                const int neighbor_radius = (self.info && self.info->NeighborSearchRadius > 0)
                    ? self.info->NeighborSearchRadius
                    : 0;
                const int radius = resolve_search_radius(
                    neighbor_radius,
                    furthest_checkpoint_distance_px,
                    assets->max_impassable_query_radius());
                assets->query_impassable_entries(self, radius, entries);
            }
        }
        return entries;
    }

    const std::vector<CollisionEntryRef>& collisions_if_loaded() const {
        return entries;
    }
};
