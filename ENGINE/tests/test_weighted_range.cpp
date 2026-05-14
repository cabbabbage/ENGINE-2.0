#include <doctest/doctest.h>

#include <random>

#include "utils/weighted_range.hpp"

TEST_CASE("Weighted range non-looping resolve overload matches clamped legacy resolve") {
    auto range = vibble::weighted_range::make_legacy_uniform(10, 20);
    std::mt19937 legacy_rng(1234);
    std::mt19937 bounded_rng(1234);

    for (int i = 0; i < 128; ++i) {
        const auto legacy = vibble::weighted_range::resolve(range, legacy_rng);
        const auto bounded = vibble::weighted_range::resolve(range, bounded_rng, 0, 100, false);
        CHECK(bounded == legacy);
        CHECK(bounded >= 10);
        CHECK(bounded <= 20);
    }
}

TEST_CASE("Weighted range wraps values into inclusive loop domain") {
    CHECK(vibble::weighted_range::wrap_inclusive(181, -180, 180) == -180);
    CHECK(vibble::weighted_range::wrap_inclusive(182, -180, 180) == -179);
    CHECK(vibble::weighted_range::wrap_inclusive(-181, -180, 180) == 180);
    CHECK(vibble::weighted_range::wrap_inclusive(540, -180, 180) == 179);
}

TEST_CASE("Weighted degree ranges resolve across the loop boundary") {
    vibble::weighted_range::WeightedIntRange range;
    range.random = true;
    range.center = 170;
    range.span = 30;
    range.falloff = 15;
    range.weights = vibble::weighted_range::WeightedRangeWeights{1.0, 1.0, 1.0};

    std::mt19937 rng(99);
    bool saw_wrapped_negative = false;
    bool saw_positive = false;
    for (int i = 0; i < 512; ++i) {
        const auto resolved = vibble::weighted_range::resolve(range, rng, -180, 180, true);
        CHECK(resolved >= -180);
        CHECK(resolved <= 180);
        saw_wrapped_negative = saw_wrapped_negative || resolved < -160;
        saw_positive = saw_positive || resolved > 150;
    }
    CHECK(saw_wrapped_negative);
    CHECK(saw_positive);
}
