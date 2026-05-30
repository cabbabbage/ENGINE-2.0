#include "gameplay/map_generation/coarseness_system.hpp"

#include "gameplay/map_generation/room.hpp"
#include "utils/area.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
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

} // namespace

void apply_coarseness_expansion(std::vector<Room*>& rooms) {
    std::mt19937 rng(std::random_device{}());
    for (Room* room : rooms) {
        if (!room || !room->room_area) continue;
        const int coarseness = read_coarseness(room);
        if (coarseness <= 0) continue;

        const std::vector<SDL_Point> original_boundary = room->room_area->get_points();
        if (original_boundary.size() < 3) continue;
        const int stride = std::max(1, 10 - (coarseness / 120));
        int expansions = 0;
        for (std::size_t i = 0; i < original_boundary.size(); i += static_cast<std::size_t>(stride)) {
            const SDL_Point center = original_boundary[i];
            const int radius = resolve_radius_from_coarseness(coarseness, rng);
            if (expand_with_circle(room, rooms, center, radius)) {
                ++expansions;
            }
        }
        vibble::log::debug(std::string("[Coarseness] room='") + room->room_name +
                           "' coarseness=" + std::to_string(coarseness) +
                           " expansions=" + std::to_string(expansions));
    }
}

} // namespace vibble::mapgen::coarseness
