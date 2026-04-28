#include <doctest/doctest.h>

#include "gameplay/map_generation/generate_trails.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

bool points_equal(const SDL_Point& a, const SDL_Point& b) {
    return a.x == b.x && a.y == b.y;
}

double normalize_angle_degrees(double value) {
    double normalized = std::fmod(value, 360.0);
    if (normalized < 0.0) {
        normalized += 360.0;
    }
    if (normalized >= 360.0) {
        normalized -= 360.0;
    }
    return normalized;
}

double angle_degrees_from_center(const SDL_Point& center, const SDL_Point& point) {
    const double dx = static_cast<double>(point.x - center.x);
    const double dy = static_cast<double>(point.y - center.y);
    const double angle_rad = std::atan2(dx, -dy);
    return normalize_angle_degrees(angle_rad * (180.0 / 3.14159265358979323846));
}

double shortest_angular_distance_degrees(double a, double b) {
    const double na = normalize_angle_degrees(a);
    const double nb = normalize_angle_degrees(b);
    double delta = std::abs(na - nb);
    if (delta > 180.0) {
        delta = 360.0 - delta;
    }
    return delta;
}

bool angle_in_sector(double angle_deg, double direction_deg, int width_percent) {
    const int clamped_width = std::clamp(width_percent, 25, 100);
    const double span = 360.0 * static_cast<double>(clamped_width) / 100.0;
    if (span >= 359.999) {
        return true;
    }
    return shortest_angular_distance_degrees(angle_deg, direction_deg) <= span * 0.5 + 1e-6;
}

std::vector<SDL_Point> make_rect(int x, int y, int w, int h) {
    return {
        SDL_Point{x, y},
        SDL_Point{x + w, y},
        SDL_Point{x + w, y + h},
        SDL_Point{x, y + h},
    };
}

bool point_in_polygon(const SDL_Point& point, const std::vector<SDL_Point>& polygon) {
    if (polygon.size() < 3) {
        return false;
    }
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto& a = polygon[i];
        const auto& b = polygon[j];
        const bool intersect =
            ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (static_cast<double>(b.x - a.x) * static_cast<double>(point.y - a.y)) /
                               (static_cast<double>(b.y - a.y) + 1e-9) +
                           static_cast<double>(a.x));
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

std::unique_ptr<Room> make_room_with_sector(const std::string& name,
                                            const std::vector<SDL_Point>& polygon,
                                            double direction_deg,
                                            int width_percent) {
    Area precomputed(name, polygon, 3);
    auto room = std::make_unique<Room>(
        Room::Point{0, 0},
        "room",
        name,
        nullptr,
        "test_map",
        nullptr,
        &precomputed,
        nullptr,
        MapGridSettings::defaults(),
        5000.0,
        "rooms_data",
        nullptr,
        nullptr,
        std::string{},
        Room::ManifestWriter{},
        false);
    nlohmann::json& data = room->assets_data();
    data["trail_connection_sector"] = nlohmann::json::object({
        {"direction_deg", direction_deg},
        {"width_percent", width_percent},
    });
    return room;
}

SDL_Point nearest_polygon_point_to_center(const Room& trail_room, const SDL_Point& center) {
    SDL_Point nearest{0, 0};
    double best_dist = std::numeric_limits<double>::infinity();
    for (const SDL_Point& point : trail_room.room_area->get_points()) {
        const double dx = static_cast<double>(point.x - center.x);
        const double dy = static_cast<double>(point.y - center.y);
        const double dist = dx * dx + dy * dy;
        if (dist < best_dist) {
            best_dist = dist;
            nearest = point;
        }
    }
    return nearest;
}

int count_trail_vertices_inside_room(const Room& trail_room, const Room& room) {
    if (!trail_room.room_area || !room.room_area) {
        return 0;
    }
    int count = 0;
    for (const SDL_Point& point : trail_room.room_area->get_points()) {
        if (room.room_area->contains_point(point)) {
            ++count;
        }
    }
    return count;
}

bool trail_respects_room_sector(const Room& room, const Room& trail_room) {
    const nlohmann::json& data = room.assets_data();
    if (!data.is_object() || !data.contains("trail_connection_sector") || !data["trail_connection_sector"].is_object()) {
        return false;
    }
    const auto& sector = data["trail_connection_sector"];
    const double direction = sector.value("direction_deg", 0.0);
    const int width_percent = sector.value("width_percent", 100);
    const SDL_Point center = room.room_area->get_center();
    const SDL_Point contact = nearest_polygon_point_to_center(trail_room, center);
    const double angle = angle_degrees_from_center(center, contact);
    return angle_in_sector(angle, direction, width_percent);
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

TEST_CASE("trail sector contacts with direction 270 and width 50 stay on left half") {
    const SDL_Point center{1200, 1200};
    std::vector<SDL_Point> contacts;
    const bool ok = trail_generation::debug::collect_sector_contacts_for_circle_tests(
        center,
        200,
        trail_generation::debug::TrailSectorDebug{270.0, 50},
        SDL_Point{1800, 1200},
        &contacts);

    REQUIRE(ok);
    REQUIRE_FALSE(contacts.empty());
    for (const SDL_Point& point : contacts) {
        CHECK(point.x <= center.x);
        CHECK(trail_generation::debug::point_in_sector_for_tests(
            center, point, trail_generation::debug::TrailSectorDebug{270.0, 50}));
    }
}

TEST_CASE("routed centerline avoids connected room interiors when direct path is blocked") {
    const std::vector<SDL_Point> room_a = make_rect(-900, -220, 280, 280);
    const std::vector<SDL_Point> room_b = make_rect(620, -220, 280, 280);
    const std::vector<std::vector<SDL_Point>> blockers{
        make_rect(-140, -980, 280, 1960)
    };

    trail_generation::debug::RoutedCenterlineDebug debug_case;
    const bool ok = trail_generation::debug::build_routed_centerline_for_tests(
        room_a,
        trail_generation::debug::TrailSectorDebug{90.0, 50},
        room_b,
        trail_generation::debug::TrailSectorDebug{270.0, 50},
        blockers,
        80,
        &debug_case);

    REQUIRE(ok);
    REQUIRE(debug_case.boundary_zone_ok);
    REQUIRE(debug_case.centerline_points.size() >= 2);
    for (std::size_t i = 1; i + 1 < debug_case.centerline_points.size(); ++i) {
        CHECK_FALSE(point_in_polygon(debug_case.centerline_points[i], room_a));
        CHECK_FALSE(point_in_polygon(debug_case.centerline_points[i], room_b));
    }
}

TEST_CASE("route planner returns multi-segment polyline when straight path is blocked") {
    std::vector<SDL_Point> route_points;
    const bool ok = trail_generation::debug::build_route_polyline_for_tests(
        SDL_Point{-700, 0},
        SDL_Point{700, 0},
        {make_rect(-120, -500, 240, 1000)},
        80,
        &route_points);

    REQUIRE(ok);
    CHECK(route_points.size() > 2);
}

TEST_CASE("boundary zone validation rejects routes that immediately leave narrow sector cones") {
    const std::vector<SDL_Point> room_a = make_rect(-900, -220, 280, 280);
    const std::vector<SDL_Point> room_b = make_rect(620, -220, 280, 280);

    trail_generation::debug::RoutedCenterlineDebug debug_case;
    const bool ok = trail_generation::debug::build_routed_centerline_for_tests(
        room_a,
        trail_generation::debug::TrailSectorDebug{0.0, 25},
        room_b,
        trail_generation::debug::TrailSectorDebug{0.0, 25},
        {},
        80,
        &debug_case);

    REQUIRE(ok);
    CHECK_FALSE(debug_case.boundary_zone_ok);
}

TEST_CASE("forced trail connections obey room trail connection sectors") {
    auto room_a = make_room_with_sector("forced_a", make_rect(-920, -220, 280, 280), 90.0, 50);
    auto room_b = make_room_with_sector("forced_b", make_rect(640, -220, 280, 280), 270.0, 50);
    std::vector<Room*> all_rooms{room_a.get(), room_b.get()};

    nlohmann::json trails_data = nlohmann::json::object({
        {"default_trail", nlohmann::json::object({
             {"name", "default_trail"},
             {"min_width", 80},
             {"max_width", 80},
             {"curvyness", 0}
         })}
    });

    GenerateTrails generator(trails_data, {});
    generator.set_all_rooms_reference(all_rooms);
    std::vector<std::unique_ptr<Room>> trails = generator.generate_trails(
        {{room_a.get(), room_b.get()}},
        "test_map",
        nullptr,
        5000.0,
        nullptr,
        nullptr,
        Room::ManifestWriter{});

    REQUIRE(trails.size() == 1);
    const Room& trail_room = *trails.front();
    CHECK(trail_respects_room_sector(*room_a, trail_room));
    CHECK(trail_respects_room_sector(*room_b, trail_room));
}

TEST_CASE("random planned trail connections obey room trail connection sectors") {
    auto room_left = make_room_with_sector("random_left", make_rect(-1500, -220, 280, 280), 90.0, 50);
    auto room_mid = make_room_with_sector("random_mid", make_rect(-140, -220, 280, 280), 0.0, 100);
    auto room_right = make_room_with_sector("random_right", make_rect(1220, -220, 280, 280), 270.0, 50);
    std::vector<Room*> all_rooms{room_left.get(), room_mid.get(), room_right.get()};

    nlohmann::json trails_data = nlohmann::json::object({
        {"default_trail", nlohmann::json::object({
             {"name", "default_trail"},
             {"min_width", 70},
             {"max_width", 90},
             {"curvyness", 1}
         })}
    });

    GenerateTrails generator(trails_data, {});
    generator.set_all_rooms_reference(all_rooms);
    std::vector<std::unique_ptr<Room>> trails = generator.generate_trails(
        {},
        "test_map",
        nullptr,
        7000.0,
        nullptr,
        nullptr,
        Room::ManifestWriter{});

    REQUIRE(trails.size() >= 2);
    for (const auto& trail_ptr : trails) {
        REQUIRE(trail_ptr);
        const Room& trail_room = *trail_ptr;
        for (Room* connected : trail_room.connected_rooms) {
            REQUIRE(connected != nullptr);
            CHECK(trail_respects_room_sector(*connected, trail_room));
        }
    }
}

TEST_CASE("trail endpoints include fully contained mouth footprint inside connected rooms") {
    auto room_a = make_room_with_sector("contained_a", make_rect(-900, -220, 280, 280), 90.0, 50);
    auto room_b = make_room_with_sector("contained_b", make_rect(620, -220, 280, 280), 270.0, 50);
    std::vector<Room*> all_rooms{room_a.get(), room_b.get()};

    nlohmann::json trails_data = nlohmann::json::object({
        {"default_trail", nlohmann::json::object({
             {"name", "default_trail"},
             {"min_width", 90},
             {"max_width", 120},
             {"curvyness", 2}
         })}
    });

    GenerateTrails generator(trails_data, {});
    generator.set_all_rooms_reference(all_rooms);
    std::vector<std::unique_ptr<Room>> trails = generator.generate_trails(
        {{room_a.get(), room_b.get()}},
        "test_map",
        nullptr,
        5000.0,
        nullptr,
        nullptr,
        Room::ManifestWriter{});

    REQUIRE(trails.size() == 1);
    const Room& trail_room = *trails.front();
    CHECK(count_trail_vertices_inside_room(trail_room, *room_a) >= 3);
    CHECK(count_trail_vertices_inside_room(trail_room, *room_b) >= 3);
}
