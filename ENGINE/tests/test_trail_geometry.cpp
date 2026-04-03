#include <doctest/doctest.h>
#include "gameplay/map_generation/trail_geometry.hpp"

#include <cmath>
#include <random>

namespace {

double polygon_area(const std::vector<SDL_Point>& poly) {
    if (poly.size() < 3) {
        return 0.0;
    }
    double area = 0.0;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        area += static_cast<double>(poly[j].x) * poly[i].y;
        area -= static_cast<double>(poly[i].x) * poly[j].y;
    }
    return std::abs(area) * 0.5;
}

std::vector<SDL_Point> straight_line(int start_x, int end_x, int depth = 0) {
    return {
        SDL_Point{start_x, depth},
        SDL_Point{(start_x + end_x) / 2, depth},
        SDL_Point{end_x, depth}
    };
}

}  // namespace

TEST_CASE("TrailGeometry width envelope respects template limits") {
    std::mt19937 rng(0x42);
    const int min_width = 32;
    const int max_width = 64;
    const int curvyness = 4;
    std::vector<SDL_Point> base_line = {
        SDL_Point{0, 0},
        SDL_Point{40, 10},
        SDL_Point{80, -10},
        SDL_Point{120, 0}
    };

    trail_generation::TrailGeometryReport report;
    auto polygon = trail_generation::build_trail_polygon(base_line, min_width, max_width, curvyness, rng, &report);
    REQUIRE_FALSE(polygon.empty());
    REQUIRE_EQ(report.local_widths.size(), report.centerline.size());
    for (double width : report.local_widths) {
        CHECK(width >= static_cast<double>(min_width));
        CHECK(width <= static_cast<double>(max_width));
    }
}

TEST_CASE("TrailGeometry produces valid polygon with positive area") {
    std::mt19937 rng(0x1337);
    const int min_width = 40;
    const int max_width = 88;
    const int curvyness = 6;
    std::vector<SDL_Point> base_line = {
        SDL_Point{0, 0},
        SDL_Point{30, 15},
        SDL_Point{60, -10},
        SDL_Point{90, 25},
        SDL_Point{120, 0}
    };

    trail_generation::TrailGeometryReport report;
    auto polygon = trail_generation::build_trail_polygon(base_line, min_width, max_width, curvyness, rng, &report);
    REQUIRE(polygon.size() > 4);
    int minx = polygon[0].x;
    int maxx = polygon[0].x;
    int miny = polygon[0].y;
    int maxy = polygon[0].y;
    for (const auto& point : polygon) {
        minx = std::min(minx, point.x);
        maxx = std::max(maxx, point.x);
        miny = std::min(miny, point.y);
        maxy = std::max(maxy, point.y);
    }
    CHECK(maxx > minx);
    CHECK(maxy > miny);
    CHECK(polygon_area(polygon) > 0.0);
    REQUIRE_EQ(report.boundary_points, polygon.size());
}

TEST_CASE("TrailGeometry deterministic runtime with fixed seed") {
    std::vector<SDL_Point> base_line = {
        SDL_Point{0, 0},
        SDL_Point{400, 0}
    };
    const int min_width = 50;
    const int max_width = 80;
    const int curvyness = 10;
    std::mt19937 rng(0xC0DE);
    trail_generation::TrailGeometryReport report_a;
    auto polygon_a = trail_generation::build_trail_polygon(base_line, min_width, max_width, curvyness, rng, &report_a);
    REQUIRE_FALSE(polygon_a.empty());
    CHECK(report_a.resampled_points <= trail_generation::kTrailGeometryMaxSamples);

    std::mt19937 rng_b(0xC0DE);
    trail_generation::TrailGeometryReport report_b;
    auto polygon_b = trail_generation::build_trail_polygon(base_line, min_width, max_width, curvyness, rng_b, &report_b);
    REQUIRE_EQ(polygon_a.size(), polygon_b.size());
    for (size_t i = 0; i < polygon_a.size(); ++i) {
        CHECK(polygon_a[i].x == polygon_b[i].x);
        CHECK(polygon_a[i].y == polygon_b[i].y);
    }
    CHECK(report_b.boundary_points == polygon_b.size());
    CHECK(report_a.resampled_points == report_b.resampled_points);
}
