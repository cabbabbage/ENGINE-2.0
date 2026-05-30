#include "gameplay/map_generation/coarseness_system.hpp"

#include "gameplay/map_generation/room.hpp"
#include "utils/area.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <random>
#include <string>
#include <vector>

#ifdef VIBBLE_HAS_CLIPPER2
#include <clipper2/clipper.h>
#endif

namespace vibble::mapgen::coarseness {
namespace {

constexpr int kMinCoarseness = 0;
constexpr int kMaxCoarseness = 1000;

int read_coarseness(const Room* room) {
    if (!room) return 0;
    const auto& data = room->assets_data();
    int value = 0;
    if (data.contains("coarseness")) {
        const auto& j = data["coarseness"];
        if (j.is_number_integer()) value = j.get<int>();
        else if (j.is_number_float()) value = static_cast<int>(std::lround(j.get<double>()));
    }
    return std::clamp(value, kMinCoarseness, kMaxCoarseness);
}

int resolve_radius_from_coarseness(int coarseness, std::mt19937& rng) {
    const int clamped = std::clamp(coarseness, kMinCoarseness, kMaxCoarseness);
    const int min_r = std::max(8, 12 + (clamped / 20));
    const int max_r = std::max(min_r, 24 + (clamped / 6));
    std::uniform_int_distribution<int> dist(min_r, max_r);
    return dist(rng);
}

std::vector<SDL_Point> make_circle_polygon(SDL_Point center, int radius, int segments = 24) {
    constexpr double kTwoPi = 6.28318530717958647692;
    std::vector<SDL_Point> out;
    segments = std::max(8, segments);
    out.reserve(static_cast<std::size_t>(segments));
    for (int i = 0; i < segments; ++i) {
        const double t = (kTwoPi * static_cast<double>(i)) / static_cast<double>(segments);
        out.push_back(SDL_Point{
            center.x + static_cast<int>(std::lround(std::cos(t) * static_cast<double>(radius))),
            center.y + static_cast<int>(std::lround(std::sin(t) * static_cast<double>(radius)))
        });
    }
    return out;
}

bool point_inside_other_geometry(SDL_Point p, const Room* owner, const std::vector<Room*>& all_rooms) {
    for (const Room* other : all_rooms) {
        if (!other || other == owner || !other->room_area) continue;
        if (other->room_area->contains_point(p)) return true;
    }
    return false;
}

#ifdef VIBBLE_HAS_CLIPPER2
Clipper2Lib::PathD to_path(const std::vector<SDL_Point>& points) {
    Clipper2Lib::PathD out;
    out.reserve(points.size());
    for (const auto& p : points) {
        out.push_back(Clipper2Lib::PointD{static_cast<double>(p.x), static_cast<double>(p.y)});
    }
    return out;
}

std::vector<SDL_Point> to_points(const Clipper2Lib::PathD& path) {
    std::vector<SDL_Point> out;
    out.reserve(path.size());
    for (const auto& p : path) {
        out.push_back(SDL_Point{
            static_cast<int>(std::lround(p.x)),
            static_cast<int>(std::lround(p.y))
        });
    }
    return out;
}
#endif

bool expand_with_circle(Room* room, const std::vector<Room*>& all_rooms, SDL_Point center, int radius) {
    if (!room || !room->room_area) return false;
    if (point_inside_other_geometry(center, room, all_rooms)) return false;

    Area circle_area("coarseness_circle", make_circle_polygon(center, radius), room->room_area->resolution());
    circle_area.set_type(room->room_area->get_type());

#ifdef VIBBLE_HAS_CLIPPER2
    try {
        Clipper2Lib::PathsD subject;
        subject.push_back(to_path(room->room_area->get_points()));
        Clipper2Lib::PathsD clip;
        clip.push_back(to_path(circle_area.get_points()));
        Clipper2Lib::PathsD added = Clipper2Lib::Difference(clip, subject, Clipper2Lib::FillRule::NonZero, 2);
        if (added.empty()) {
            return false;
        }
        Clipper2Lib::PathsD grown = Clipper2Lib::Union(subject, added, Clipper2Lib::FillRule::NonZero, 2);
        if (grown.empty() || grown.front().size() < 3) {
            return false;
        }
        room->room_area = std::make_unique<Area>(room->room_name, to_points(grown.front()), room->room_area->resolution());
        room->room_area->set_type(room->type);
        if (!room->coarseness_added_area) {
            room->coarseness_added_area = std::make_unique<Area>(room->room_name + "_coarse_added", to_points(added.front()), room->room_area->resolution());
        } else {
            room->coarseness_added_area->union_with(Area("coarse_piece", to_points(added.front()), room->room_area->resolution()));
        }
        return true;
    } catch (const std::exception& ex) {
        vibble::log::warn(std::string("[Coarseness] Boolean expansion failed for room '") + room->room_name + "': " + ex.what());
        return false;
    }
#else
    std::vector<SDL_Point> added_points;
    for (const auto& p : circle_area.get_points()) {
        if (!room->room_area->contains_point(p)) {
            added_points.push_back(p);
        }
    }
    if (added_points.size() < 3) return false;
    Area added("coarse_added_piece", added_points, room->room_area->resolution());
    room->room_area->union_with(added);
    if (!room->coarseness_added_area) {
        room->coarseness_added_area = std::make_unique<Area>(room->room_name + "_coarse_added", added.get_points(), added.resolution());
    } else {
        room->coarseness_added_area->union_with(added);
    }
    return true;
#endif
}

double distance_sq(SDL_Point a, SDL_Point b) {
    const double dx = static_cast<double>(a.x - b.x);
    const double dy = static_cast<double>(a.y - b.y);
    return dx * dx + dy * dy;
}

int orientation(SDL_Point a, SDL_Point b, SDL_Point c) {
    const long long v = static_cast<long long>(b.y - a.y) * static_cast<long long>(c.x - b.x) -
                        static_cast<long long>(b.x - a.x) * static_cast<long long>(c.y - b.y);
    if (v == 0) return 0;
    return v > 0 ? 1 : 2;
}

bool on_segment(SDL_Point a, SDL_Point b, SDL_Point c) {
    return b.x <= std::max(a.x, c.x) && b.x >= std::min(a.x, c.x) &&
           b.y <= std::max(a.y, c.y) && b.y >= std::min(a.y, c.y);
}

bool segments_intersect(SDL_Point p1, SDL_Point q1, SDL_Point p2, SDL_Point q2) {
    const int o1 = orientation(p1, q1, p2);
    const int o2 = orientation(p1, q1, q2);
    const int o3 = orientation(p2, q2, p1);
    const int o4 = orientation(p2, q2, q1);
    if (o1 != o2 && o3 != o4) return true;
    if (o1 == 0 && on_segment(p1, p2, q1)) return true;
    if (o2 == 0 && on_segment(p1, q2, q1)) return true;
    if (o3 == 0 && on_segment(p2, p1, q2)) return true;
    if (o4 == 0 && on_segment(p2, q1, q2)) return true;
    return false;
}

std::vector<SDL_Point> boundary_intersections(const std::vector<SDL_Point>& circle_poly,
                                              const std::vector<SDL_Point>& boundary) {
    std::vector<SDL_Point> out;
    if (circle_poly.size() < 2 || boundary.size() < 2) return out;
    for (std::size_t i = 0; i < circle_poly.size(); ++i) {
        const SDL_Point a0 = circle_poly[i];
        const SDL_Point a1 = circle_poly[(i + 1) % circle_poly.size()];
        for (std::size_t j = 0; j < boundary.size(); ++j) {
            const SDL_Point b0 = boundary[j];
            const SDL_Point b1 = boundary[(j + 1) % boundary.size()];
            if (segments_intersect(a0, a1, b0, b1)) {
                out.push_back(SDL_Point{
                    (a0.x + a1.x + b0.x + b1.x) / 4,
                    (a0.y + a1.y + b0.y + b1.y) / 4
                });
            }
        }
    }
    return out;
}

SDL_Point choose_next_center(Room* room,
                             const std::vector<Room*>& all_rooms,
                             SDL_Point current_center,
                             const std::vector<SDL_Point>& previous_circle) {
    if (!room || !room->room_area) return current_center;
    const std::vector<SDL_Point> boundary = room->room_area->get_points();
    if (boundary.empty()) return current_center;

    std::vector<SDL_Point> candidates = previous_circle.empty()
        ? boundary
        : boundary_intersections(previous_circle, boundary);
    if (candidates.empty()) {
        candidates = boundary;
    }

    SDL_Point best = current_center;
    double best_dist = -1.0;
    for (const SDL_Point& p : candidates) {
        if (point_inside_other_geometry(p, room, all_rooms)) continue;
        const double d2 = distance_sq(current_center, p);
        if (d2 > best_dist) {
            best_dist = d2;
            best = p;
        }
    }
    return best;
}

} // namespace

void apply_coarseness_expansion(std::vector<Room*>& rooms) {
    std::mt19937 rng(std::random_device{}());
    for (Room* room : rooms) {
        if (!room || !room->room_area) continue;
        const int coarseness = read_coarseness(room);
        if (coarseness <= 0) continue;

        const std::vector<SDL_Point> original_boundary = room->room_area->get_points();
        if (original_boundary.size() < 3) continue;
        SDL_Point cursor = original_boundary.front();
        std::vector<SDL_Point> previous_circle;
        const int max_steps = std::max<int>(24, static_cast<int>(original_boundary.size()) * 2);
        int expansions = 0;
        for (int step = 0; step < max_steps; ++step) {
            const SDL_Point center = cursor;
            const int radius = resolve_radius_from_coarseness(coarseness, rng);
            previous_circle = make_circle_polygon(center, radius);
            if (expand_with_circle(room, rooms, center, radius)) {
                ++expansions;
            }
            cursor = choose_next_center(room, rooms, center, previous_circle);
        }
        vibble::log::debug(std::string("[Coarseness] room='") + room->room_name +
                           "' coarseness=" + std::to_string(coarseness) +
                           " expansions=" + std::to_string(expansions));
    }
}

} // namespace vibble::mapgen::coarseness
