#include "gameplay/spawn/dynamic_spawn_geometry.hpp"

#include <algorithm>

#include "utils/area.hpp"

namespace dynamic_spawn::geometry {
namespace {
double point_to_segment_distance_sq(SDL_Point p, SDL_Point a, SDL_Point b) {
    const double px = static_cast<double>(p.x), py = static_cast<double>(p.y);
    const double ax = static_cast<double>(a.x), ay = static_cast<double>(a.y);
    const double bx = static_cast<double>(b.x), by = static_cast<double>(b.y);
    const double vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const double len_sq = vx * vx + vy * vy;
    double t = 0.0;
    if (len_sq > 1.0e-9) t = std::clamp((wx * vx + wy * vy) / len_sq, 0.0, 1.0);
    const double cx = ax + t * vx, cy = ay + t * vy;
    const double dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy;
}
}

bool point_inside_any_area(SDL_Point point, const AreaGeometry& geometry) {
    for (const Area* area : geometry.areas) {
        if (area && area->contains_point(point)) return true;
    }
    return false;
}

bool point_near_geometry(SDL_Point point, const AreaGeometry& geometry, int threshold_px) {
    if (point_inside_any_area(point, geometry)) return true;
    const double threshold_sq = static_cast<double>(std::max(0, threshold_px)) * static_cast<double>(std::max(0, threshold_px));
    for (const AreaGeometry::Segment& segment : geometry.segments) {
        if (point_to_segment_distance_sq(point, segment.a, segment.b) <= threshold_sq) return true;
    }
    return false;
}

} // namespace dynamic_spawn::geometry
