#include <doctest/doctest.h>

#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

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

TEST_CASE("DynamicBoundarySystem depth efficiency keep ratio is linear between efficiency depth and cull depth") {
    const double max_cull_depth = 1000.0;
    const double efficiency_depth = 400.0;
    const float min_density_ratio = 0.10f;

    CHECK(DynamicBoundarySystem::compute_depth_efficiency_keep_ratio(150.0,
                                                                     max_cull_depth,
                                                                     efficiency_depth,
                                                                     min_density_ratio) == doctest::Approx(1.0f));
    CHECK(DynamicBoundarySystem::compute_depth_efficiency_keep_ratio(400.0,
                                                                     max_cull_depth,
                                                                     efficiency_depth,
                                                                     min_density_ratio) == doctest::Approx(1.0f));
    CHECK(DynamicBoundarySystem::compute_depth_efficiency_keep_ratio(700.0,
                                                                     max_cull_depth,
                                                                     efficiency_depth,
                                                                     min_density_ratio) == doctest::Approx(0.55f));
    CHECK(DynamicBoundarySystem::compute_depth_efficiency_keep_ratio(1000.0,
                                                                     max_cull_depth,
                                                                      efficiency_depth,
                                                                      min_density_ratio) == doctest::Approx(0.10f));
}

TEST_CASE("DynamicBoundarySystem depth efficiency keep ratio keeps near-camera side fully visible") {
    const double max_cull_depth = 1000.0;
    const double efficiency_depth = 400.0;
    const float min_density_ratio = 0.10f;

    CHECK(DynamicBoundarySystem::compute_depth_efficiency_keep_ratio(-250.0,
                                                                     max_cull_depth,
                                                                     efficiency_depth,
                                                                     min_density_ratio) == doctest::Approx(1.0f));
    CHECK(DynamicBoundarySystem::compute_depth_efficiency_keep_ratio(0.0,
                                                                     max_cull_depth,
                                                                     efficiency_depth,
                                                                     min_density_ratio) == doctest::Approx(1.0f));
}

TEST_CASE("DynamicBoundarySystem forward depth offset conversion follows camera forward sign") {
    CHECK(DynamicBoundarySystem::compute_forward_depth_offset(-200.0, 1.0f) == doctest::Approx(200.0));
    CHECK(DynamicBoundarySystem::compute_forward_depth_offset(200.0, -1.0f) == doctest::Approx(200.0));
    CHECK(DynamicBoundarySystem::compute_forward_depth_offset(200.0, 1.0f) == doctest::Approx(-200.0));
    CHECK(std::isnan(DynamicBoundarySystem::compute_forward_depth_offset(
        std::numeric_limits<double>::quiet_NaN(), 1.0f)));
}

TEST_CASE("DynamicBoundarySystem depth efficiency sampling is deterministic and respects extremes") {
    const std::uint64_t key_hash = 0xDEADBEEF12345678ULL;
    const bool sample_a = DynamicBoundarySystem::should_keep_depth_efficiency_sample(key_hash, 0.35f);
    const bool sample_b = DynamicBoundarySystem::should_keep_depth_efficiency_sample(key_hash, 0.35f);
    CHECK(sample_a == sample_b);
    CHECK_FALSE(DynamicBoundarySystem::should_keep_depth_efficiency_sample(key_hash, 0.0f));
    CHECK(DynamicBoundarySystem::should_keep_depth_efficiency_sample(key_hash, 1.0f));
}

TEST_CASE("DynamicBoundarySystem depth efficiency visibility hysteresis reduces threshold flicker") {
    constexpr float sample = 0.50f;
    constexpr float keep_ratio = 0.52f;
    constexpr float hysteresis = 0.05f;

    CHECK_FALSE(DynamicBoundarySystem::evaluate_depth_efficiency_visibility(
        sample, keep_ratio, false, hysteresis));
    CHECK(DynamicBoundarySystem::evaluate_depth_efficiency_visibility(
        sample, keep_ratio, true, hysteresis));

    CHECK_FALSE(DynamicBoundarySystem::evaluate_depth_efficiency_visibility(
        sample, 0.44f, true, hysteresis));
    CHECK(DynamicBoundarySystem::evaluate_depth_efficiency_visibility(
        sample, 1.0f, false, hysteresis));
    CHECK_FALSE(DynamicBoundarySystem::evaluate_depth_efficiency_visibility(
        sample, 0.0f, true, hysteresis));
}

TEST_CASE("DynamicBoundarySystem promoted candidate eligibility matches efficiency lifecycle policy") {
    CHECK(DynamicBoundarySystem::should_promote_controller_candidate(true, true, 120.0, 300.0));
    CHECK_FALSE(DynamicBoundarySystem::should_promote_controller_candidate(true, true, 301.0, 300.0));
    CHECK_FALSE(DynamicBoundarySystem::should_promote_controller_candidate(true, true, -1.0, 300.0));
    CHECK_FALSE(DynamicBoundarySystem::should_promote_controller_candidate(false, true, 120.0, 300.0));
    CHECK_FALSE(DynamicBoundarySystem::should_promote_controller_candidate(true, false, 120.0, 300.0));
}

TEST_CASE("DynamicBoundarySystem frame advancement freezes and resumes without catch-up") {
    std::vector<DynamicBoundarySystem::BoundaryFrame> frames(2);
    frames[0].duration_ms = 50.0f;
    frames[1].duration_ms = 50.0f;

    DynamicBoundarySystem::FrameState state{};
    DynamicBoundarySystem::advance_frame_state(state, frames, 120.0f, false);
    CHECK(state.frame_index == 0);
    CHECK(state.elapsed_ms == doctest::Approx(20.0f));

    DynamicBoundarySystem::advance_frame_state(state, frames, 300.0f, true);
    CHECK(state.frame_index == 0);
    CHECK(state.elapsed_ms == doctest::Approx(20.0f));

    DynamicBoundarySystem::advance_frame_state(state, frames, 40.0f, false);
    CHECK(state.frame_index == 1);
    CHECK(state.elapsed_ms == doctest::Approx(10.0f));
}

TEST_CASE("DynamicBoundarySystem room trail promotion gate rejects boundary-domain controller promotion") {
    CHECK(DynamicBoundarySystem::should_attempt_room_trail_promotion(1, true));
    CHECK_FALSE(DynamicBoundarySystem::should_attempt_room_trail_promotion(0, true));
    CHECK_FALSE(DynamicBoundarySystem::should_attempt_room_trail_promotion(1, false));
}

TEST_CASE("DynamicBoundarySystem origin slot reservation retention keeps slot occupied while in scope and alive") {
    CHECK_FALSE(DynamicBoundarySystem::should_release_origin_slot_reservation(true, true));
}

TEST_CASE("DynamicBoundarySystem origin slot reservation releases when out of scope or asset invalid") {
    CHECK(DynamicBoundarySystem::should_release_origin_slot_reservation(false, true));
    CHECK(DynamicBoundarySystem::should_release_origin_slot_reservation(true, false));
    CHECK(DynamicBoundarySystem::should_release_origin_slot_reservation(false, false));
}

TEST_CASE("DynamicBoundarySystem persistent promoted policy keeps assets alive despite domain drift") {
    // Domain drift is modeled by keeping the promoted asset valid while origin slot remains in scope.
    CHECK_FALSE(DynamicBoundarySystem::should_release_origin_slot_reservation(true, true));
}
