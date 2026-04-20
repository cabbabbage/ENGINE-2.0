#include "unstick_utils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/area.hpp"

namespace {

SDL_Point nearest_point_on_segment(SDL_Point point, SDL_Point a, SDL_Point b) {
    const double ax = static_cast<double>(a.x);
    const double ay = static_cast<double>(a.y);
    const double bx = static_cast<double>(b.x);
    const double by = static_cast<double>(b.y);
    const double px = static_cast<double>(point.x);
    const double py = static_cast<double>(point.y);

    const double abx = bx - ax;
    const double aby = by - ay;
    const double ab_sq = abx * abx + aby * aby;
    if (ab_sq <= 0.0) {
        return a;
    }

    const double apx = px - ax;
    const double apy = py - ay;
    const double t = std::clamp((apx * abx + apy * aby) / ab_sq, 0.0, 1.0);

    return SDL_Point{
        static_cast<int>(std::lround(ax + abx * t)),
        static_cast<int>(std::lround(ay + aby * t))
    };
}

SDL_Point nearest_perimeter_point(SDL_Point point, const Area& area) {
    const auto& pts = area.get_points();
    if (pts.empty()) {
        return area.get_center();
    }
    if (pts.size() == 1) {
        return pts.front();
    }

    SDL_Point best = pts.front();
    long long best_dist = std::numeric_limits<long long>::max();

    for (std::size_t i = 0; i < pts.size(); ++i) {
        const SDL_Point a = pts[i];
        const SDL_Point b = pts[(i + 1) % pts.size()];
        const SDL_Point candidate = nearest_point_on_segment(point, a, b);
        const long long dx = static_cast<long long>(candidate.x) - point.x;
        const long long dy = static_cast<long long>(candidate.y) - point.y;
        const long long dist = dx * dx + dy * dy;
        if (dist < best_dist) {
            best_dist = dist;
            best = candidate;
        }
    }

    return best;
}

std::vector<SDL_Point> build_escape_directions(SDL_Point primary) {
    std::vector<SDL_Point> dirs;
    dirs.reserve(5);
    auto add = [&](SDL_Point d) {
        if (d.x == 0 && d.y == 0) {
            return;
        }
        for (const auto& existing : dirs) {
            if (existing.x == d.x && existing.y == d.y) {
                return;
            }
        }
        dirs.push_back(d);
    };

    if (primary.x == 0 && primary.y == 0) {
        dirs = {{1,0},{-1,0},{0,1},{0,-1}};
    } else {
        add(primary);
        add({primary.x, 0});
        add({0, primary.y});
        add({primary.y, -primary.x});
        add({-primary.y, primary.x});
    }

    return dirs;
}

} // namespace

namespace animation::unstick {

bool resolve_destination(const Asset& self,
                         const Assets* assets,
                         const std::vector<CollisionEntryRef>& entries,
                         const world::GridPoint& start,
                         world::GridPoint& out_destination,
                         int max_steps) {
    const world::GridPoint start_bottom = animation_update::detail::bottom_middle_for(self, start);

    std::vector<CollisionEntryRef> containing;
    containing.reserve(entries.size());
    SDL_Point push_vector{0, 0};

    for (const CollisionEntryRef entry : entries) {
        if (!entry || !entry->asset || entry->asset == &self || !entry->asset->info) {
            continue;
        }
        if (!entry->area.contains_point(start_bottom.to_sdl_point())) {
            continue;
        }

        containing.push_back(entry);
        const SDL_Point perimeter = nearest_perimeter_point(start_bottom.to_sdl_point(), entry->area);
        push_vector.x += perimeter.x - start_bottom.world_x();
        push_vector.y += perimeter.y - start_bottom.world_z();
    }

    if (containing.empty()) {
        return false;
    }

    if (push_vector.x == 0 && push_vector.y == 0) {
        push_vector = {1, 0};
    }

    const SDL_Point primary_dir{
        (push_vector.x > 0) ? 1 : ((push_vector.x < 0) ? -1 : 0),
        (push_vector.y > 0) ? 1 : ((push_vector.y < 0) ? -1 : 0)
    };

    const auto inside_any = [&](const world::GridPoint& bottom) {
        for (const CollisionEntryRef entry : containing) {
            if (!entry) {
                continue;
            }
            if (entry->area.contains_point(bottom.to_sdl_point())) {
                return true;
            }
        }
        return false;
    };

    for (const SDL_Point dir : build_escape_directions(primary_dir)) {
        world::GridPoint cursor = start;
        bool moved = false;
        for (int step = 0; step < std::max(1, max_steps); ++step) {
            const world::GridPoint next = world::grid_math::offset(cursor, dir);
            if (next.world_x() == cursor.world_x() && next.world_z() == cursor.world_z()) {
                continue;
            }

            const world::GridPoint bottom_next = animation_update::detail::bottom_middle_for(self, next);
            if (!animation_update::detail::bottom_point_inside_playable_area(assets, bottom_next)) {
                break;
            }
            if (inside_any(bottom_next)) {
                cursor = next;
                moved = true;
                continue;
            }

            cursor = next;
            moved = true;
            out_destination = cursor;
            return true;
        }

        if (moved) {
            out_destination = cursor;
            return true;
        }
    }

    return false;
}

bool push_out_of_impassable(Asset& self,
                            const Assets* assets,
                            const std::vector<CollisionEntryRef>& entries,
                            int max_steps) {
    const world::GridPoint start = world::GridPoint::make_virtual(
        self.world_x(),
        self.world_y(),
        self.world_z(),
        self.grid_resolution);
    world::GridPoint destination = start;
    if (!resolve_destination(self, assets, entries, start, destination, max_steps)) {
        return false;
    }

    if (destination.world_x() == start.world_x() && destination.world_z() == start.world_z()) {
        return false;
    }

    self.move_to_world_position(destination.world_x(), self.world_y(), destination.world_z());
    return true;
}

} // namespace animation::unstick
