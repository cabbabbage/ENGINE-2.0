#pragma once

#include "utils/weighted_range.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <utility>

namespace vibble::coarseness {

inline constexpr int kMinCoarseness = 0;
inline constexpr int kMaxCoarseness = 4000;
inline constexpr int kMinCoarsenessRadius = 8;

vibble::weighted_range::WeightedIntRange from_legacy_value(int value);
vibble::weighted_range::WeightedIntRange normalize_range_value(
    const nlohmann::json& value,
    const vibble::weighted_range::WeightedIntRange& fallback = vibble::weighted_range::make_flat(0));
std::optional<vibble::weighted_range::WeightedIntRange> read_optional_range(const nlohmann::json& owner,
                                                                            const char* key = "coarseness");
bool enabled(const vibble::weighted_range::WeightedIntRange& range);
std::pair<int, int> bounds(const vibble::weighted_range::WeightedIntRange& range);

}  // namespace vibble::coarseness
