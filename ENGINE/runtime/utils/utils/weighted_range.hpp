#pragma once

#include <cstdint>
#include <random>

#include <nlohmann/json.hpp>

namespace vibble::weighted_range {

struct WeightedRangeWeights {
    double edge = 1.0;
    double falloff = 1.0;
    double center = 1.0;
};

struct WeightedIntRange {
    bool random = false;
    std::int64_t center = 0;
    std::int64_t span = 0;
    std::int64_t falloff = 0;
    WeightedRangeWeights weights{};
};

bool is_valid(const WeightedIntRange& value);
WeightedIntRange make_flat(std::int64_t value);
WeightedIntRange make_legacy_uniform(std::int64_t min_value, std::int64_t max_value);
WeightedIntRange from_json(const nlohmann::json& value, const WeightedIntRange& fallback = make_flat(0));
nlohmann::json to_json(const WeightedIntRange& value);
std::int64_t resolve(const WeightedIntRange& value, std::mt19937& rng);
std::int64_t wrap_inclusive(std::int64_t value, std::int64_t min_value, std::int64_t max_value);
std::int64_t resolve(const WeightedIntRange& value,
                     std::mt19937& rng,
                     std::int64_t min_allowed,
                     std::int64_t max_allowed,
                     bool loop);
std::int64_t resolve(const nlohmann::json& value,
                     std::mt19937& rng,
                     const WeightedIntRange& fallback = make_flat(0));
std::int64_t normalize_signed_degrees(std::int64_t value);

}  // namespace vibble::weighted_range
