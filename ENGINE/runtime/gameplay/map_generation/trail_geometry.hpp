#pragma once

#include <SDL3/SDL.h>
#include <cstddef>
#include <random>
#include <vector>

namespace trail_generation {

constexpr size_t kTrailGeometryMaxSamples = 512;
constexpr int kTrailGeometryBoundarySmoothPasses = 2;

struct TrailGeometryReport {
    struct Point {
        double x = 0.0;
        double y = 0.0;
    };
    std::vector<Point> centerline;
    std::vector<double> local_widths;
    std::vector<double> left_half_widths;
    std::vector<double> right_half_widths;
    std::vector<Point> left_boundary;
    std::vector<Point> right_boundary;
    size_t resampled_points = 0;
    size_t boundary_points = 0;
    size_t smoothing_passes = 0;
};

std::vector<SDL_Point> build_trail_polygon(const std::vector<SDL_Point>& base_centerline,
                                           int min_width,
                                           int max_width,
                                           int curvyness,
                                           std::mt19937& rng,
                                           TrailGeometryReport* report = nullptr);

bool polygons_overlap_precise(const std::vector<SDL_Point>& a, const std::vector<SDL_Point>& b);

}  // namespace trail_generation
