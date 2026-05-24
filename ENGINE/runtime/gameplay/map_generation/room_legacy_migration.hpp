#pragma once

#include <functional>
#include <nlohmann/json.hpp>

#include "utils/weighted_range.hpp"

namespace room_legacy_migration {

struct DimensionRanges {
    vibble::weighted_range::WeightedIntRange width;
    vibble::weighted_range::WeightedIntRange height;
    bool used_legacy_migration = false;
};

// Compatibility-only bridge for pre-manifest room shape fields.
// Sunset criteria: remove once all shipped content uses manifest-native width/height
// weighted ranges and no runtime payloads contain radius/min_* /max_* legacy keys.
DimensionRanges resolve_dimension_ranges(const nlohmann::json& assets_json,
                                         const vibble::weighted_range::WeightedIntRange& default_range,
                                         const std::function<void(const char*)>& on_legacy_migration);

}  // namespace room_legacy_migration
