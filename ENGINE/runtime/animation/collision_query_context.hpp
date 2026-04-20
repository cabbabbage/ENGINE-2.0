#pragma once

#include <vector>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"

struct CollisionQueryContext {
    using CollisionEntryRef = const Assets::FrameCollisionEntry*;

    std::vector<CollisionEntryRef> entries{};
    bool loaded = false;

    const std::vector<CollisionEntryRef>& collisions_for(const Asset& self) {
        if (!loaded) {
            loaded = true;
            const Assets* assets = self.get_assets();
            if (assets) {
                const int search_radius = (self.info && self.info->NeighborSearchRadius > 0)
                    ? self.info->NeighborSearchRadius
                    : 0;
                assets->query_impassable_entries(self, search_radius, entries);
            }
        }
        return entries;
    }

    const std::vector<CollisionEntryRef>& collisions_if_loaded() const {
        return entries;
    }
};

