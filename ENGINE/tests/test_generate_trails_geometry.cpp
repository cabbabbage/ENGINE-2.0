#include <doctest/doctest.h>

#include "gameplay/map_generation/generate_trails.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

bool points_equal(const SDL_Point& a, const SDL_Point& b) {
    return a.x == b.x && a.y == b.y;
}

} // namespace

TEST_CASE("trail layout always includes cross-sections at both centerline edge points") {
    trail_generation::debug::TrailLayoutDebug layout;
    const bool ok = trail_generation::debug::build_layout_for_tests(
        SDL_Point{10, 20},
        SDL_Point{120, 20},
        80,
        120,
        3,
        {},
        1337u,
        &layout);

    REQUIRE(ok);
    REQUIRE(layout.sections.size() >= 2);
    CHECK(doctest::Approx(layout.sections.front().distance_along_centerline).epsilon(1e-6) == 0.0);

    const double total_length = std::hypot(static_cast<double>(layout.end_tip.x - layout.start_tip.x),
                                           static_cast<double>(layout.end_tip.y - layout.start_tip.y));
    CHECK(doctest::Approx(layout.sections.back().distance_along_centerline).epsilon(1e-6) == total_length);
}

TEST_CASE("trail section widths remain inside configured min/max range") {
    trail_generation::debug::TrailLayoutDebug layout;
    const bool ok = trail_generation::debug::build_layout_for_tests(
        SDL_Point{0, 0},
        SDL_Point{1000, 0},
        140,
        220,
        4,
        {},
        41u,
        &layout);

    REQUIRE(ok);
    REQUIRE_FALSE(layout.sections.empty());
    for (const auto& section : layout.sections) {
        CHECK(section.width >= 140);
        CHECK(section.width <= 220);
    }
}

TEST_CASE("trail section shifts are unique and bounded by section half-width") {
    trail_generation::debug::TrailLayoutDebug layout;
    const bool ok = trail_generation::debug::build_layout_for_tests(
        SDL_Point{0, 0},
        SDL_Point{1000, 300},
        160,
        320,
        6,
        {},
        2026u,
        &layout);

    REQUIRE(ok);
    REQUIRE(layout.sections.size() > 2);

    for (std::size_t i = 0; i < layout.sections.size(); ++i) {
        const auto& section = layout.sections[i];
        const double half_width = static_cast<double>(section.width) * 0.5;
        CHECK(std::abs(section.shift) <= half_width);
        for (std::size_t j = i + 1; j < layout.sections.size(); ++j) {
            CHECK(std::abs(section.shift - layout.sections[j].shift) > 0.01);
        }
    }
}

TEST_CASE("trail polygon order follows tip-left-chain-end-tip-right-chain") {
    trail_generation::debug::TrailLayoutDebug layout;
    const bool ok = trail_generation::debug::build_layout_for_tests(
        SDL_Point{50, -40},
        SDL_Point{850, 120},
        120,
        220,
        5,
        {},
        99u,
        &layout);

    REQUIRE(ok);
    const std::size_t n = layout.sections.size();
    REQUIRE(n >= 2);
    REQUIRE(layout.polygon.size() == 2 + 2 * n);

    CHECK(points_equal(layout.polygon.front(), layout.start_tip));
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(points_equal(layout.polygon[1 + i], layout.sections[i].left));
    }
    CHECK(points_equal(layout.polygon[1 + n], layout.end_tip));
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(points_equal(layout.polygon[2 + n + i], layout.sections[n - 1 - i].right));
    }
}

TEST_CASE("trail layout is rejected when it overlaps an existing trail polygon") {
    std::vector<SDL_Point> blocker_points{
        SDL_Point{200, -140},
        SDL_Point{800, -140},
        SDL_Point{800, 140},
        SDL_Point{200, 140},
    };
    Area blocker("existing_trail", blocker_points, 3);
    std::vector<Area> blockers{blocker};

    trail_generation::debug::TrailLayoutDebug layout;
    const bool ok = trail_generation::debug::build_layout_for_tests(
        SDL_Point{0, 0},
        SDL_Point{1000, 0},
        220,
        220,
        0,
        blockers,
        7u,
        &layout);

    CHECK_FALSE(ok);
}
