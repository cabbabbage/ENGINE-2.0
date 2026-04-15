#include <doctest/doctest.h>

#include <cstdint>
#include <cmath>
#include <unordered_set>

#include "rendering/render/dynamic_boundary_system.hpp"
#include "assets/asset/asset_info.hpp"

TEST_CASE("DynamicBoundarySystem size variation sample is deterministic and bounded") {
    const std::uint64_t key_hash = 0x12345678ABCDEF00ULL;
    const float sample_a = DynamicBoundarySystem::sample_size_variation_from_hash(key_hash);
    const float sample_b = DynamicBoundarySystem::sample_size_variation_from_hash(key_hash);
    CHECK(sample_a == doctest::Approx(sample_b));
    CHECK(sample_a >= -1.0f);
    CHECK(sample_a <= 1.0f);
}

TEST_CASE("DynamicBoundarySystem size variation sample differs across grid-cell hashes") {
    std::unordered_set<int> quantized_samples;
    for (std::uint64_t i = 0; i < 16; ++i) {
        const float sample = DynamicBoundarySystem::sample_size_variation_from_hash(0xA550000000000000ULL + i);
        const int quantized = static_cast<int>(std::lround(sample * 1000.0f));
        quantized_samples.insert(quantized);
    }
    CHECK(quantized_samples.size() > 1);
}

TEST_CASE("DynamicBoundarySystem effective base scale keeps zero variation behavior unchanged") {
    AssetInfo info("dynamic_boundary_zero_variation");
    info.set_scale_factor(1.5f);
    info.set_size_variation_percentage(0.0f);

    const float scaled = DynamicBoundarySystem::compute_effective_base_scale(info, -0.95f);
    CHECK(scaled == doctest::Approx(1.5f));
}

TEST_CASE("DynamicBoundarySystem effective base scale applies variation and clamps percent range") {
    AssetInfo info("dynamic_boundary_variation");
    info.set_scale_factor(2.0f);
    info.set_size_variation_percentage(99.0f); // clamps to 20%

    const float larger = DynamicBoundarySystem::compute_effective_base_scale(info, 1.0f);
    const float smaller = DynamicBoundarySystem::compute_effective_base_scale(info, -1.0f);
    CHECK(larger == doctest::Approx(2.4f));
    CHECK(smaller == doctest::Approx(1.6f));
}

TEST_CASE("DynamicBoundarySystem effective base scale ignores variation for tillable assets") {
    AssetInfo info("dynamic_boundary_tillable");
    info.set_scale_factor(1.75f);
    info.set_size_variation_percentage(20.0f);
    info.set_tillable(true);

    const float scaled = DynamicBoundarySystem::compute_effective_base_scale(info, 1.0f);
    CHECK(scaled == doctest::Approx(1.75f));
}
