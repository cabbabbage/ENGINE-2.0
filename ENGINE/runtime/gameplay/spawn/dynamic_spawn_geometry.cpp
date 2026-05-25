#include "gameplay/spawn/dynamic_spawn_geometry.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/spawn/trail_classification.hpp"
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

namespace {
bool named_area_is_trail(const Room::NamedArea& area) {
    return is_trail_area_label(area.type) ||
           is_trail_area_label(area.kind) ||
           is_trail_area_label(area.name);
}
} // namespace

AreaGeometry collect_area_geometry(const Assets& assets) {
    AreaGeometry geometry;
    geometry.min_x = std::numeric_limits<int>::max();
    geometry.min_z = std::numeric_limits<int>::max();
    geometry.max_x = std::numeric_limits<int>::min();
    geometry.max_z = std::numeric_limits<int>::min();

    auto append_area = [&](const Area* area) {
        if (!area) return;
        geometry.areas.push_back(area);
        const auto& points = area->get_points();
        if (points.empty()) return;
        geometry.valid = true;
        for (const SDL_Point& point : points) {
            geometry.min_x = std::min(geometry.min_x, point.x);
            geometry.min_z = std::min(geometry.min_z, point.y);
            geometry.max_x = std::max(geometry.max_x, point.x);
            geometry.max_z = std::max(geometry.max_z, point.y);
        }
        if (points.size() == 2) {
            geometry.segments.push_back(AreaGeometry::Segment{points[0], points[1]});
        } else if (points.size() > 2) {
            for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
                geometry.segments.push_back(AreaGeometry::Segment{points[j], points[i]});
            }
        }
    };

    for (Room* room : assets.rooms()) {
        if (!room) continue;
        append_area(room->room_area.get());
        for (const Room::NamedArea& area : room->areas) {
            if (named_area_is_trail(area)) append_area(area.area.get());
        }
    }

    if (!geometry.valid) {
        geometry.min_x = geometry.min_z = 0;
        geometry.max_x = geometry.max_z = -1;
    }
    return geometry;
}

} // namespace dynamic_spawn::geometry
