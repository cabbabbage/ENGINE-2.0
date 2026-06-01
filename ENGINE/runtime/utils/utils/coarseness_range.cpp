#include "utils/coarseness_range.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vibble::coarseness {
namespace {

std::optional<int> json_to_int(const nlohmann::json& value) {
    try {
        if (value.is_number_integer()) {
            return value.get<int>();
        }
        if (value.is_number_unsigned()) {
            const auto raw = value.get<std::uint64_t>();
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                return std::numeric_limits<int>::max();
            }
            return static_cast<int>(raw);
        }
        if (value.is_number_float()) {
            const double raw = value.get<double>();
            if (!std::isfinite(raw)) {
                return std::nullopt;
            }
            return static_cast<int>(std::clamp(std::llround(raw),
                                               static_cast<long long>(std::numeric_limits<int>::min()),
                                               static_cast<long long>(std::numeric_limits<int>::max())));
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text.empty()) {
                return std::nullopt;
            }
            std::size_t idx = 0;
            const long parsed = std::stol(text, &idx, 10);
            if (idx == text.size()) {
                return static_cast<int>(std::clamp<long>(parsed,
                                                         std::numeric_limits<int>::min(),
                                                         std::numeric_limits<int>::max()));
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

}  // namespace

vibble::weighted_range::WeightedIntRange from_legacy_value(int value) {
    const int clamped = std::clamp(value, kMinCoarseness, kMaxCoarseness);
    if (clamped <= 0) {
        return vibble::weighted_range::make_flat(0);
    }
    const int min_radius = std::max(kMinCoarsenessRadius, 12 + (clamped / 18));
    const int max_radius = std::max(min_radius, 36 + (clamped / 4));
    return vibble::weighted_range::make_legacy_uniform(min_radius, max_radius);
}

vibble::weighted_range::WeightedIntRange normalize_range_value(
    const nlohmann::json& value,
    const vibble::weighted_range::WeightedIntRange& fallback) {
    if (const auto legacy = json_to_int(value)) {
        return from_legacy_value(*legacy);
    }
    const auto parsed = vibble::weighted_range::from_json(value, fallback);
    if (!vibble::weighted_range::is_valid(parsed)) {
        return fallback;
    }
    return parsed;
}

std::optional<vibble::weighted_range::WeightedIntRange> read_optional_range(const nlohmann::json& owner,
                                                                            const char* key) {
    if (!owner.is_object()) {
        return std::nullopt;
    }
    auto it = owner.find(key);
    if (it == owner.end() || it->is_null() || (it->is_object() && it->empty())) {
        return std::nullopt;
    }
    return normalize_range_value(*it);
}

bool enabled(const vibble::weighted_range::WeightedIntRange& range) {
    if (!vibble::weighted_range::is_valid(range)) {
        return false;
    }
    return range.center + std::llabs(range.span) > 0;
}

std::pair<int, int> bounds(const vibble::weighted_range::WeightedIntRange& range) {
    const std::int64_t span = std::llabs(range.span);
    std::int64_t min_value = range.center - span;
    std::int64_t max_value = range.center + span;
    if (min_value > max_value) {
        std::swap(min_value, max_value);
    }
    min_value = std::clamp<std::int64_t>(min_value, 0, std::numeric_limits<int>::max());
    max_value = std::clamp<std::int64_t>(max_value, 0, std::numeric_limits<int>::max());
    min_value = std::min(min_value, max_value);
    return {static_cast<int>(min_value), static_cast<int>(max_value)};
}

}  // namespace vibble::coarseness
