#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "spawn_info.hpp"

inline std::unordered_set<std::string> collect_spacing_asset_names(
    const std::vector<SpawnInfo>& queue,
    const vibble::spawn::RuntimeCandidates::AssetCatalogView& catalog) {
    std::unordered_set<std::string> names;
    names.reserve(queue.size());
    for (const auto& item : queue) {
        if (!item.check_min_spacing) {
            continue;
        }
        item.candidates.append_positive_asset_names(names, catalog);
    }
    return names;
}
