#include <doctest/doctest.h>

#include "animation/controllers/custom_controllers/anchor_binding_order.hpp"
#include "rendering/render/render_depth_policy.hpp"

TEST_CASE("Anchor binding order keeps parent before chained children") {
    const std::vector<anchor_binding_order::Node> nodes{
        anchor_binding_order::Node{1, std::nullopt, 10},
        anchor_binding_order::Node{2, 1, 20},
        anchor_binding_order::Node{3, 2, 30}
    };

    const anchor_binding_order::Result result = anchor_binding_order::compute(nodes);
    CHECK_FALSE(result.has_cycle);
    REQUIRE(result.ordered_ids.size() == 3);
    CHECK(result.ordered_ids[0] == 1);
    CHECK(result.ordered_ids[1] == 2);
    CHECK(result.ordered_ids[2] == 3);
}

TEST_CASE("Anchor binding order is deterministic for fan-out children") {
    const std::vector<anchor_binding_order::Node> nodes{
        anchor_binding_order::Node{10, std::nullopt, 1},
        anchor_binding_order::Node{20, 10, 2},
        anchor_binding_order::Node{30, 10, 3}
    };

    const anchor_binding_order::Result result = anchor_binding_order::compute(nodes);
    CHECK_FALSE(result.has_cycle);
    REQUIRE(result.ordered_ids.size() == 3);
    CHECK(result.ordered_ids[0] == 10);
    CHECK(result.ordered_ids[1] == 20);
    CHECK(result.ordered_ids[2] == 30);
}

TEST_CASE("Anchor binding order reports cycles and appends a stable fallback order") {
    const std::vector<anchor_binding_order::Node> nodes{
        anchor_binding_order::Node{100, 200, 1},
        anchor_binding_order::Node{200, 100, 2}
    };

    const anchor_binding_order::Result result = anchor_binding_order::compute(nodes);
    CHECK(result.has_cycle);
    CHECK(result.cycle_nodes == 2);
    REQUIRE(result.ordered_ids.size() == 2);
    CHECK(result.ordered_ids[0] == 100);
    CHECK(result.ordered_ids[1] == 200);
}

TEST_CASE("Quantized depth bias restores exact render ordering") {
    constexpr double anchor_depth = 100.0;
    constexpr double exact_object_depth = 12.25;
    constexpr double quantized_object_depth = 12.0;

    const double biased_depth = render_depth::depth_from_anchor(
        anchor_depth,
        quantized_object_depth,
        render_depth::bias_for_quantized_depth(exact_object_depth, quantized_object_depth));
    const double exact_depth = render_depth::depth_from_anchor(anchor_depth, exact_object_depth);

    CHECK(biased_depth == doctest::Approx(exact_depth).epsilon(1e-9));
}
