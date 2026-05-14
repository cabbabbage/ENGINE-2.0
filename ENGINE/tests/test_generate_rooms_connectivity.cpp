#include <doctest/doctest.h>

#include "gameplay/map_generation/generate_rooms.hpp"

#include <algorithm>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

nlohmann::json room_template(const std::string& name, int size = 180) {
    return nlohmann::json::object({
        {"name", name},
        {"geometry", "Square"},
        {"min_width", size},
        {"max_width", size},
        {"min_height", size},
        {"max_height", size},
        {"edge_smoothness", 1},
        {"trail_connection_sector", nlohmann::json::object({
            {"direction_deg", 0.0},
            {"width_percent", 100},
        })},
    });
}

nlohmann::json trail_template() {
    return nlohmann::json::object({
        {"name", "default_trail"},
        {"min_width", 60},
        {"max_width", 80},
        {"curvyness", 1},
        {"edge_smoothness", 1},
    });
}

std::vector<LayerSpec> connected_layers() {
    return {
        LayerSpec{0, 1, {RoomSpec{"spawn", 1, {}}}},
        LayerSpec{1, 2, {
            RoomSpec{"room_a", 1, {"room_c"}},
            RoomSpec{"room_b", 1, {}},
        }},
        LayerSpec{2, 0, {}},
    };
}

bool is_trail(const Room* room) {
    return room && room->type == "trail";
}

bool all_non_trail_rooms_reachable(Room* spawn, const std::vector<std::unique_ptr<Room>>& rooms) {
    std::unordered_set<Room*> seen;
    std::queue<Room*> pending;
    seen.insert(spawn);
    pending.push(spawn);
    while (!pending.empty()) {
        Room* current = pending.front();
        pending.pop();
        for (Room* next : current->connected_rooms) {
            if (next && seen.insert(next).second) {
                pending.push(next);
            }
        }
    }
    for (const auto& room : rooms) {
        if (room && !is_trail(room.get()) && seen.find(room.get()) == seen.end()) {
            return false;
        }
    }
    return true;
}

bool connected_to(const Room* trail, const Room* room) {
    if (!trail || !room) {
        return false;
    }
    return std::find(trail->connected_rooms.begin(), trail->connected_rooms.end(), room) != trail->connected_rooms.end();
}

} // namespace

TEST_CASE("generated connected_rooms graph reaches every non-trail room from spawn") {
    nlohmann::json manifest = nlohmann::json::object();
    nlohmann::json rooms_data = nlohmann::json::object({
        {"spawn", room_template("spawn", 220)},
        {"room_a", room_template("room_a", 180)},
        {"room_b", room_template("room_b", 180)},
        {"room_c", room_template("room_c", 160)},
    });
    nlohmann::json trails_data = nlohmann::json::object({
        {"default_trail", trail_template()},
    });

    GenerateRooms generator(connected_layers(), 0, 0, "connectivity_test", manifest, 120.0);
    std::vector<double> layer_radii{0.0, 700.0, 1250.0};
    auto rooms = generator.build(
        nullptr,
        2600.0,
        layer_radii,
        nlohmann::json::object(),
        rooms_data,
        trails_data,
        MapGridSettings::defaults());

    REQUIRE_FALSE(rooms.empty());
    REQUIRE(all_non_trail_rooms_reachable(rooms.front().get(), rooms));
}

TEST_CASE("final trail areas do not overlap unrelated room areas") {
    nlohmann::json manifest = nlohmann::json::object();
    nlohmann::json rooms_data = nlohmann::json::object({
        {"spawn", room_template("spawn", 220)},
        {"room_a", room_template("room_a", 180)},
        {"room_b", room_template("room_b", 180)},
        {"room_c", room_template("room_c", 160)},
    });
    nlohmann::json trails_data = nlohmann::json::object({
        {"default_trail", trail_template()},
    });

    GenerateRooms generator(connected_layers(), 0, 0, "overlap_test", manifest, 120.0);
    std::vector<double> layer_radii{0.0, 700.0, 1250.0};
    auto rooms = generator.build(
        nullptr,
        2600.0,
        layer_radii,
        nlohmann::json::object(),
        rooms_data,
        trails_data,
        MapGridSettings::defaults());

    REQUIRE_FALSE(rooms.empty());
    for (const auto& trail : rooms) {
        if (!trail || !is_trail(trail.get()) || !trail->room_area) {
            continue;
        }
        for (const auto& room : rooms) {
            if (!room || is_trail(room.get()) || !room->room_area || connected_to(trail.get(), room.get())) {
                continue;
            }
            CHECK_FALSE(trail->room_area->intersects(*room->room_area));
        }
    }
}
