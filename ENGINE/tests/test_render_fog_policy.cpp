#include <doctest/doctest.h>

#include <algorithm>
#include <numeric>
#include <vector>

#include "rendering/render/render.hpp"

TEST_CASE("Render fog policy enables fog only for layers behind the player") {
    constexpr int kForegroundLayers = 3;
    constexpr int kBackgroundLayers = 4;
    constexpr int kPlayerLayer = 3; // first background layer

    CHECK_FALSE(render_internal::should_apply_background_layer_fog(2,
                                                                    kForegroundLayers,
                                                                    kBackgroundLayers,
                                                                    kPlayerLayer,
                                                                    false));
    CHECK_FALSE(render_internal::should_apply_background_layer_fog(3,
                                                                    kForegroundLayers,
                                                                    kBackgroundLayers,
                                                                    kPlayerLayer,
                                                                    false));
    CHECK(render_internal::should_apply_background_layer_fog(4,
                                                             kForegroundLayers,
                                                             kBackgroundLayers,
                                                             kPlayerLayer,
                                                             false));
    CHECK(render_internal::should_apply_background_layer_fog(6,
                                                             kForegroundLayers,
                                                             kBackgroundLayers,
                                                             kPlayerLayer,
                                                             false));
}

TEST_CASE("Render fog policy disables fog in single-layer fallback mode") {
    CHECK_FALSE(render_internal::should_apply_background_layer_fog(4,
                                                                    3,
                                                                    4,
                                                                    3,
                                                                    true));
}

TEST_CASE("Render fog policy clamps fog bottom to player floor") {
    CHECK(render_internal::clamp_fog_bottom_to_player_floor(70.0f, 50.0f, 100) == doctest::Approx(50.0f));
    CHECK(render_internal::clamp_fog_bottom_to_player_floor(70.0f, 120.0f, 100) == doctest::Approx(70.0f));
    CHECK(render_internal::clamp_fog_bottom_to_player_floor(-20.0f, 30.0f, 100) == doctest::Approx(0.0f));
}

TEST_CASE("Render fog policy assigns deterministic cycle index per background segment") {
    CHECK(render_internal::fog_cycle_index_for_background_segment(0) == 0);
    CHECK(render_internal::fog_cycle_index_for_background_segment(1) == 1);
    CHECK(render_internal::fog_cycle_index_for_background_segment(5) == 5);
    CHECK(render_internal::fog_cycle_index_for_background_segment(6) == 6);
    CHECK(render_internal::fog_cycle_index_for_background_segment(-3) == 0);
}

TEST_CASE("Render layer chains keep player only in background_mid chain") {
    const std::vector<int> non_empty_layers{0, 1, 2, 3, 4, 5, 6};
    constexpr int kPlayerLayer = 3;

    const std::vector<int> background_chain =
        render_internal::background_chain_layers(non_empty_layers, kPlayerLayer);
    const std::vector<int> foreground_chain =
        render_internal::foreground_chain_layers(non_empty_layers, kPlayerLayer);

    const std::vector<int> expected_background{6, 5, 4, 3};
    const std::vector<int> expected_foreground{0, 1, 2};
    CHECK(background_chain == expected_background);
    CHECK(foreground_chain == expected_foreground);
}

TEST_CASE("Render foreground chain excludes all layers at or behind player") {
    const std::vector<int> non_empty_layers{1, 4, 7, 9, 11};
    constexpr int kPlayerLayer = 7;

    const std::vector<int> foreground_chain =
        render_internal::foreground_chain_layers(non_empty_layers, kPlayerLayer);
    const std::vector<int> expected_foreground{1, 4};
    CHECK(foreground_chain == expected_foreground);
}

TEST_CASE("Render blur repeat distribution evenly assigns passes and preserves exact total") {
    const std::vector<int> repeats =
        render_internal::distributed_blur_repeat_counts(6, 4);
    REQUIRE(repeats.size() == 4);
    CHECK(repeats == std::vector<int>{1, 2, 1, 2});
    CHECK(std::accumulate(repeats.begin(), repeats.end(), 0) == 6);
    CHECK(*std::max_element(repeats.begin(), repeats.end()) -
          *std::min_element(repeats.begin(), repeats.end()) <= 1);
}

TEST_CASE("Render blur repeat distribution supports fewer passes than layers") {
    const std::vector<int> repeats =
        render_internal::distributed_blur_repeat_counts(3, 5);
    REQUIRE(repeats.size() == 5);
    CHECK(repeats == std::vector<int>{0, 1, 0, 1, 1});
    CHECK(std::accumulate(repeats.begin(), repeats.end(), 0) == 3);
    CHECK(*std::max_element(repeats.begin(), repeats.end()) -
          *std::min_element(repeats.begin(), repeats.end()) <= 1);
}

TEST_CASE("Render blur repeat distribution handles single-layer and zero-target edge cases") {
    CHECK(render_internal::distributed_blur_repeat_counts(0, 0).empty());
    CHECK(render_internal::distributed_blur_repeat_counts(0, 1) == std::vector<int>{0});
    CHECK(render_internal::distributed_blur_repeat_counts(7, 1) == std::vector<int>{7});
}

TEST_CASE("Render chain pass equalization excludes player boundary from background blur budget") {
    const std::vector<int> non_empty_layers{0, 1, 2, 3, 4, 5, 6};
    constexpr int kPlayerLayer = 3;
    const std::vector<int> background_chain =
        render_internal::background_chain_layers(non_empty_layers, kPlayerLayer);
    const std::vector<int> foreground_chain =
        render_internal::foreground_chain_layers(non_empty_layers, kPlayerLayer);

    const std::size_t background_eligible = static_cast<std::size_t>(std::count_if(
        background_chain.begin(),
        background_chain.end(),
        [kPlayerLayer](int layer_index) { return layer_index != kPlayerLayer; }));
    const std::size_t foreground_eligible = foreground_chain.size();
    const std::size_t target_passes = std::max(background_eligible, foreground_eligible);

    CHECK(background_eligible == 3);
    CHECK(foreground_eligible == 3);
    CHECK(target_passes == 3);

    const std::vector<int> background_repeats =
        render_internal::distributed_blur_repeat_counts(target_passes, background_eligible);
    const std::vector<int> foreground_repeats =
        render_internal::distributed_blur_repeat_counts(target_passes, foreground_eligible);
    CHECK(std::accumulate(background_repeats.begin(), background_repeats.end(), 0) == static_cast<int>(target_passes));
    CHECK(std::accumulate(foreground_repeats.begin(), foreground_repeats.end(), 0) == static_cast<int>(target_passes));
}

TEST_CASE("DoF quality scale threshold radii remain stable at 2 6 12 and 24 px") {
    constexpr int kW = 1920;
    constexpr int kH = 1080;
    CHECK(render_internal::dof_quality_scale(kW, kH, 2.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(render_internal::dof_quality_scale(kW, kH, 6.0f, 0.0f) == doctest::Approx(0.92f));
    CHECK(render_internal::dof_quality_scale(kW, kH, 12.0f, 0.0f) == doctest::Approx(0.84f));
    CHECK(render_internal::dof_quality_scale(kW, kH, 24.0f, 0.0f) == doctest::Approx(0.72f));
    CHECK(render_internal::dof_quality_scale(kW, kH, 25.0f, 0.0f) == doctest::Approx(0.54f));
}
