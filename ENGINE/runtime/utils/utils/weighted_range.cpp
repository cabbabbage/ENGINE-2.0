#include "weighted_range.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace vibble::weighted_range {

namespace {

constexpr double kEpsilon = 1e-12;

std::optional<std::int64_t> parse_integer_like(const nlohmann::json& value) {
    try {
        if (value.is_number_integer()) {
            return value.get<std::int64_t>();
        }
        if (value.is_number_unsigned()) {
            return static_cast<std::int64_t>(value.get<std::uint64_t>());
        }
        if (value.is_number_float()) {
            return static_cast<std::int64_t>(std::llround(value.get<double>()));
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text.empty()) {
                return std::nullopt;
            }
            std::size_t idx = 0;
            const long long parsed = std::stoll(text, &idx, 10);
            if (idx == text.size()) {
                return static_cast<std::int64_t>(parsed);
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<double> parse_weight_like(const nlohmann::json& value) {
    try {
        if (value.is_number()) {
            return value.get<double>();
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text.empty()) {
                return std::nullopt;
            }
            std::size_t idx = 0;
            const double parsed = std::stod(text, &idx);
            if (idx == text.size()) {
                return parsed;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

double sanitize_weight(double value, double fallback) {
    if (!std::isfinite(value) || value < 0.0) {
        return fallback;
    }
    return value;
}

WeightedRangeWeights normalize_weights(const WeightedRangeWeights& in, bool random) {
    if (!random) {
        return WeightedRangeWeights{0.0, 0.0, 1.0};
    }

    WeightedRangeWeights out{
        sanitize_weight(in.edge, 1.0),
        sanitize_weight(in.falloff, 1.0),
        sanitize_weight(in.center, 1.0),
    };
    const double total = out.edge + out.falloff + out.center;
    if (total <= kEpsilon) {
        return WeightedRangeWeights{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    }
    out.edge /= total;
    out.falloff /= total;
    out.center /= total;
    return out;
}

std::int64_t safe_midpoint(std::int64_t a, std::int64_t b) {
    return a + ((b - a) / 2);
}

double segment_mass(std::int64_t left,
                    std::int64_t right,
                    double left_weight,
                    double right_weight) {
    const std::int64_t span = right - left;
    if (span <= 0) {
        return 0.0;
    }
    const double mass = static_cast<double>(span) * (left_weight + right_weight) * 0.5;
    return std::max(0.0, mass);
}

double invert_segment(std::int64_t left,
                      std::int64_t right,
                      double left_weight,
                      double right_weight,
                      double sample_mass) {
    const double span = static_cast<double>(right - left);
    if (span <= 0.0) {
        return static_cast<double>(left);
    }
    const double slope = (right_weight - left_weight) / span;
    const double base = left_weight;
    if (std::abs(slope) <= kEpsilon) {
        if (base <= kEpsilon) {
            return static_cast<double>(left);
        }
        return static_cast<double>(left) + (sample_mass / base);
    }

    const double discriminant = std::max(0.0, (base * base) + (2.0 * slope * sample_mass));
    const double offset = (-base + std::sqrt(discriminant)) / slope;
    return static_cast<double>(left) + offset;
}

WeightedIntRange canonicalize(WeightedIntRange value) {
    value.span = std::llabs(value.span);
    value.falloff = std::llabs(value.falloff);
    if (value.falloff > value.span) {
        value.falloff = value.span;
    }
    value.weights = normalize_weights(value.weights, value.random);
    if (!value.random || value.span == 0) {
        value.random = false;
        value.span = 0;
        value.falloff = 0;
        value.weights = WeightedRangeWeights{0.0, 0.0, 1.0};
    }
    return value;
}

}  // namespace

bool is_valid(const WeightedIntRange& value) {
    return std::isfinite(value.weights.edge) &&
           std::isfinite(value.weights.falloff) &&
           std::isfinite(value.weights.center);
}

WeightedIntRange make_flat(std::int64_t value) {
    WeightedIntRange out;
    out.random = false;
    out.center = value;
    out.span = 0;
    out.falloff = 0;
    out.weights = WeightedRangeWeights{0.0, 0.0, 1.0};
    return out;
}

WeightedIntRange make_legacy_uniform(std::int64_t min_value, std::int64_t max_value) {
    if (max_value < min_value) {
        std::swap(min_value, max_value);
    }
    if (min_value == max_value) {
        return make_flat(min_value);
    }

    WeightedIntRange out;
    out.random = true;
    out.center = safe_midpoint(min_value, max_value);
    out.span = std::max<std::int64_t>(0, (max_value - min_value) / 2);
    out.falloff = out.span / 2;
    out.weights = WeightedRangeWeights{1.0, 1.0, 1.0};
    return canonicalize(out);
}

WeightedIntRange from_json(const nlohmann::json& value, const WeightedIntRange& fallback) {
    WeightedIntRange out = fallback;
    if (value.is_null()) {
        return canonicalize(out);
    }

    if (value.is_number() || value.is_string()) {
        if (const auto parsed = parse_integer_like(value)) {
            out = make_flat(*parsed);
        }
        return canonicalize(out);
    }

    if (!value.is_object()) {
        return canonicalize(out);
    }

    bool has_canonical_shape = value.contains("center") || value.contains("span") || value.contains("falloff") ||
                               value.contains("weights") || value.contains("random");
    if (has_canonical_shape) {
        if (auto it = value.find("center"); it != value.end()) {
            if (const auto parsed = parse_integer_like(*it)) {
                out.center = *parsed;
            }
        }
        if (auto it = value.find("span"); it != value.end()) {
            if (const auto parsed = parse_integer_like(*it)) {
                out.span = std::llabs(*parsed);
            }
        }
        if (auto it = value.find("falloff"); it != value.end()) {
            if (const auto parsed = parse_integer_like(*it)) {
                out.falloff = std::llabs(*parsed);
            }
        }
        if (value.contains("random")) {
            out.random = value.value("random", out.random);
        }

        if (value.contains("weights") && value["weights"].is_object()) {
            const auto& weights = value["weights"];
            if (auto it = weights.find("edge"); it != weights.end()) {
                if (const auto parsed = parse_weight_like(*it)) {
                    out.weights.edge = *parsed;
                }
            }
            if (auto it = weights.find("falloff"); it != weights.end()) {
                if (const auto parsed = parse_weight_like(*it)) {
                    out.weights.falloff = *parsed;
                }
            }
            if (auto it = weights.find("center"); it != weights.end()) {
                if (const auto parsed = parse_weight_like(*it)) {
                    out.weights.center = *parsed;
                }
            }
        }
        return canonicalize(out);
    }

    if (value.contains("min") || value.contains("max")) {
        const auto min_value = parse_integer_like(value.value("min", out.center)).value_or(out.center);
        const auto max_value = parse_integer_like(value.value("max", out.center)).value_or(min_value);
        return make_legacy_uniform(min_value, max_value);
    }

    return canonicalize(out);
}

nlohmann::json to_json(const WeightedIntRange& value) {
    const WeightedIntRange canonical = canonicalize(value);
    return nlohmann::json::object({
        {"random", canonical.random},
        {"center", canonical.center},
        {"span", canonical.span},
        {"falloff", canonical.falloff},
        {"weights", nlohmann::json::object({
            {"edge", canonical.weights.edge},
            {"falloff", canonical.weights.falloff},
            {"center", canonical.weights.center},
        })},
    });
}

std::int64_t resolve(const WeightedIntRange& value, std::mt19937& rng) {
    const WeightedIntRange canonical = canonicalize(value);
    if (!canonical.random || canonical.span <= 0) {
        return canonical.center;
    }

    const std::int64_t left = canonical.center - canonical.span;
    const std::int64_t left_falloff = canonical.center - canonical.falloff;
    const std::int64_t right_falloff = canonical.center + canonical.falloff;
    const std::int64_t right = canonical.center + canonical.span;

    const double mass_0 = segment_mass(left, left_falloff, canonical.weights.edge, canonical.weights.falloff);
    const double mass_1 = segment_mass(left_falloff, canonical.center, canonical.weights.falloff, canonical.weights.center);
    const double mass_2 = segment_mass(canonical.center, right_falloff, canonical.weights.center, canonical.weights.falloff);
    const double mass_3 = segment_mass(right_falloff, right, canonical.weights.falloff, canonical.weights.edge);
    const double total = mass_0 + mass_1 + mass_2 + mass_3;
    if (total <= kEpsilon) {
        return canonical.center;
    }

    std::uniform_real_distribution<double> dist(0.0, total);
    const double sample = dist(rng);

    double cursor = sample;
    if (cursor <= mass_0) {
        return static_cast<std::int64_t>(std::llround(invert_segment(left,
                                                                    left_falloff,
                                                                    canonical.weights.edge,
                                                                    canonical.weights.falloff,
                                                                    cursor)));
    }
    cursor -= mass_0;
    if (cursor <= mass_1) {
        return static_cast<std::int64_t>(std::llround(invert_segment(left_falloff,
                                                                    canonical.center,
                                                                    canonical.weights.falloff,
                                                                    canonical.weights.center,
                                                                    cursor)));
    }
    cursor -= mass_1;
    if (cursor <= mass_2) {
        return static_cast<std::int64_t>(std::llround(invert_segment(canonical.center,
                                                                    right_falloff,
                                                                    canonical.weights.center,
                                                                    canonical.weights.falloff,
                                                                    cursor)));
    }
    cursor -= mass_2;
    return static_cast<std::int64_t>(std::llround(invert_segment(right_falloff,
                                                                 right,
                                                                 canonical.weights.falloff,
                                                                 canonical.weights.edge,
                                                                 cursor)));
}

std::int64_t resolve(const nlohmann::json& value, std::mt19937& rng, const WeightedIntRange& fallback) {
    return resolve(from_json(value, fallback), rng);
}

std::int64_t normalize_signed_degrees(std::int64_t value) {
    std::int64_t normalized = value % 360;
    if (normalized > 180) {
        normalized -= 360;
    } else if (normalized < -180) {
        normalized += 360;
    }
    return normalized;
}

}  // namespace vibble::weighted_range
