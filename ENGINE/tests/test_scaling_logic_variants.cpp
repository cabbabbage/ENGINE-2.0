#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

#include "assets/asset/animation_frame.hpp"
#include "rendering/render/scaling_logic.hpp"

TEST_CASE("scaling logic default steps are canonical 10 variants") {
    const auto& steps = render_pipeline::ScalingLogic::DefaultScaleSteps();
    REQUIRE(steps.size() == 10);

    const std::array<int, 10> expected_percents{100, 90, 80, 70, 60, 50, 40, 30, 20, 10};
    for (std::size_t idx = 0; idx < expected_percents.size(); ++idx) {
        CHECK(render_pipeline::ScalingLogic::ScalePercent(steps, idx) == expected_percents[idx]);
    }
}

TEST_CASE("normalize variant steps enforces canonical 10 variants") {
    std::vector<float> custom_steps{0.75f, 0.5f, 0.25f};
    render_pipeline::ScalingLogic::NormalizeVariantSteps(custom_steps);

    const auto& defaults = render_pipeline::ScalingLogic::DefaultScaleSteps();
    REQUIRE(custom_steps.size() == defaults.size());
    for (std::size_t idx = 0; idx < defaults.size(); ++idx) {
        CHECK(custom_steps[idx] == doctest::Approx(defaults[idx]).epsilon(1e-6));
    }
}

TEST_CASE("animation frame base texture access is bounds-safe") {
    AnimationFrame frame;
    CHECK(frame.get_base_texture(0) == nullptr);
    CHECK(frame.get_base_texture(1) == nullptr);
    CHECK(frame.get_base_texture(-1) == nullptr);

    FrameVariant variant;
    variant.base_texture = reinterpret_cast<SDL_Texture*>(0x1);
    frame.variants.push_back(variant);

    CHECK(frame.get_base_texture(0) == reinterpret_cast<SDL_Texture*>(0x1));
    CHECK(frame.get_base_texture(10) == nullptr);
}
