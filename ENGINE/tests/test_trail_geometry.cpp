#include <doctest/doctest.h>
#include "gameplay/map_generation/trail_geometry.hpp"

#include <cmath>
#include <random>
#include <vector>

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

double cross(const SDL_Point& a, const SDL_Point& b, const SDL_Point& c) {
    const double abx = static_cast<double>(b.x - a.x);
    const double aby = static_cast<double>(b.y - a.y);
    const double acx = static_cast<double>(c.x - a.x);
    const double acy = static_cast<double>(c.y - a.y);
    return abx * acy - aby * acx;
}

bool segments_intersect(const SDL_Point& a, const SDL_Point& b, const SDL_Point& c, const SDL_Point& d) {
    const double o1 = cross(a, b, c);
    const double o2 = cross(a, b, d);
    const double o3 = cross(c, d, a);
    const double o4 = cross(c, d, b);
    return ((o1 > 0.0 && o2 < 0.0) || (o1 < 0.0 && o2 > 0.0)) &&
           ((o3 > 0.0 && o4 < 0.0) || (o3 < 0.0 && o4 > 0.0));
}

bool boundary_self_intersects(const std::vector<trail_generation::TrailGeometryReport::Point>& boundary) {
    if (boundary.size() < 4) {
        return false;
    }
    std::vector<SDL_Point> points;
    points.reserve(boundary.size());
    for (const auto& p : boundary) {
        points.push_back(SDL_Point{static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y))});
    }
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        for (size_t j = i + 2; j + 1 < points.size(); ++j) {
            if (segments_intersect(points[i], points[i + 1], points[j], points[j + 1])) {
                return true;
            }
        }
    }
    return false;
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
    REQUIRE_EQ(report_a.left_half_widths.size(), report_b.left_half_widths.size());
    for (size_t i = 0; i < report_a.left_half_widths.size(); ++i) {
        CHECK(report_a.left_half_widths[i] == doctest::Approx(report_b.left_half_widths[i]).epsilon(1e-9));
        CHECK(report_a.right_half_widths[i] == doctest::Approx(report_b.right_half_widths[i]).epsilon(1e-9));
    }
}

TEST_CASE("TrailGeometry high curvature boundaries avoid self-intersection") {
    std::mt19937 rng(0x7A11);
    std::vector<SDL_Point> base_line = {
        SDL_Point{0, 0},
        SDL_Point{35, 60},
        SDL_Point{70, -55},
        SDL_Point{105, 65},
        SDL_Point{140, -60},
        SDL_Point{180, 0}
    };

    trail_generation::TrailGeometryReport report;
    auto polygon = trail_generation::build_trail_polygon(base_line, 36, 90, 8, rng, &report);
    REQUIRE(polygon.size() > 6);
    CHECK_FALSE(boundary_self_intersects(report.left_boundary));
    CHECK_FALSE(boundary_self_intersects(report.right_boundary));
}

TEST_CASE("TrailGeometry supports asymmetric width profile within template bounds") {
    std::mt19937 rng(0x5150);
    std::vector<SDL_Point> base_line = {
        SDL_Point{0, 0},
        SDL_Point{70, 10},
        SDL_Point{140, -8},
        SDL_Point{220, 0}
    };
    const int min_width = 30;
    const int max_width = 74;
    trail_generation::TrailGeometryReport report;
    auto polygon = trail_generation::build_trail_polygon(base_line, min_width, max_width, 6, rng, &report);
    REQUIRE_FALSE(polygon.empty());
    REQUIRE_EQ(report.left_half_widths.size(), report.right_half_widths.size());
    bool has_asymmetry = false;
    for (size_t i = 0; i < report.left_half_widths.size(); ++i) {
        const double total = report.left_half_widths[i] + report.right_half_widths[i];
        CHECK(total >= static_cast<double>(min_width) - 1.0);
        CHECK(total <= static_cast<double>(max_width) + 1.0);
        if (std::abs(report.left_half_widths[i] - report.right_half_widths[i]) > 0.75) {
            has_asymmetry = true;
        }
    }
    CHECK(has_asymmetry);
}

TEST_CASE("TrailGeometry precise overlap check rejects bbox-only overlap false positive") {
    std::vector<SDL_Point> poly_a = {
        SDL_Point{0, 0},
        SDL_Point{60, 0},
        SDL_Point{60, 15},
        SDL_Point{15, 15},
        SDL_Point{15, 60},
        SDL_Point{0, 60}
    };
    std::vector<SDL_Point> poly_b = {
        SDL_Point{20, 20},
        SDL_Point{55, 20},
        SDL_Point{55, 55},
        SDL_Point{20, 55}
    };
    REQUIRE(trail_generation::polygons_overlap_precise(poly_a, poly_b));

    std::vector<SDL_Point> poly_c = {
        SDL_Point{20, 20},
        SDL_Point{55, 20},
        SDL_Point{55, 55},
        SDL_Point{20, 55}
    };
    std::vector<SDL_Point> poly_d = {
        SDL_Point{0, 0},
        SDL_Point{80, 0},
        SDL_Point{80, 10},
        SDL_Point{10, 10},
        SDL_Point{10, 80},
        SDL_Point{0, 80}
    };
    CHECK_FALSE(trail_generation::polygons_overlap_precise(poly_c, poly_d));
}
