#include <doctest/doctest.h>

#include "gameplay/map_generation/generate_rooms.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"
#include "map_layers_common.hpp"
#include "utils/weighted_range.hpp"

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

nlohmann::json weighted_range_json(int min_value, int max_value) {
    return vibble::weighted_range::to_json(
        vibble::weighted_range::make_legacy_uniform(min_value, max_value));
}

} // namespace

TEST_CASE("room layer extents use canonical weighted upper bounds") {
    nlohmann::json rooms_data = nlohmann::json::object({
        {"weighted_square", nlohmann::json::object({
            {"name", "weighted_square"},
            {"geometry", "Square"},
            {"width", weighted_range_json(500, 40000)},
            {"height", weighted_range_json(500, 40000)},
        })},
    });

    const double extent = map_layers::room_extent_from_rooms_data(&rooms_data, "weighted_square");

    CHECK(extent == doctest::Approx(std::sqrt(40000.0 * 40000.0 + 40000.0 * 40000.0) * 0.5));
    CHECK(extent > 70.71);
}

TEST_CASE("room layer extents preserve legacy numeric max dimensions") {
    nlohmann::json rooms_data = nlohmann::json::object({
        {"legacy_square", nlohmann::json::object({
            {"name", "legacy_square"},
            {"geometry", "Square"},
            {"min_width", 120},
            {"max_width", 300},
            {"min_height", 200},
            {"max_height", 400},
        })},
    });

    const double extent = map_layers::room_extent_from_rooms_data(&rooms_data, "legacy_square");

    CHECK(extent == doctest::Approx(std::sqrt(300.0 * 300.0 + 400.0 * 400.0) * 0.5));
}

TEST_CASE("circle room layer extents use weighted diameter upper bounds") {
    nlohmann::json rooms_data = nlohmann::json::object({
        {"weighted_circle", nlohmann::json::object({
            {"name", "weighted_circle"},
            {"geometry", "Circle"},
            {"width", weighted_range_json(200, 1200)},
            {"height", weighted_range_json(200, 1200)},
        })},
    });

    const double extent = map_layers::room_extent_from_rooms_data(&rooms_data, "weighted_circle");

    CHECK(extent == doctest::Approx(600.0));
}

TEST_CASE("weighted room extents expand computed layer radii") {
    nlohmann::json rooms_data = nlohmann::json::object({
        {"spawn", nlohmann::json::object({
            {"name", "spawn"},
            {"geometry", "Square"},
            {"width", weighted_range_json(18000, 18000)},
            {"height", weighted_range_json(12000, 12000)},
        })},
        {"loot", nlohmann::json::object({
            {"name", "loot"},
            {"geometry", "Square"},
            {"width", weighted_range_json(500, 40000)},
            {"height", weighted_range_json(500, 40000)},
        })},
    });
    nlohmann::json layers = nlohmann::json::array({
        nlohmann::json::object({
            {"level", 0},
            {"max_rooms", 1},
            {"rooms", nlohmann::json::array({nlohmann::json::object({{"name", "spawn"}, {"max_instances", 1}})})},
        }),
        nlohmann::json::object({
            {"level", 1},
            {"max_rooms", 1},
            {"rooms", nlohmann::json::array({nlohmann::json::object({{"name", "loot"}, {"max_instances", 1}})})},
        }),
    });

    const map_layers::LayerRadiiResult result = map_layers::compute_layer_radii(layers, &rooms_data, 1000.0);

    REQUIRE(result.layer_radii.size() == 2);
    CHECK(result.layer_extents[0] == doctest::Approx(std::sqrt(18000.0 * 18000.0 + 12000.0 * 12000.0) * 0.5));
    CHECK(result.layer_extents[1] == doctest::Approx(std::sqrt(40000.0 * 40000.0 + 40000.0 * 40000.0) * 0.5));
    CHECK(result.layer_radii[1] > 40000.0);
}

TEST_CASE("map layer room creation helper writes weighted defaults") {
    nlohmann::json map_info = nlohmann::json::object();

    const std::string key = map_layers::create_room_entry(map_info);

    REQUIRE(key == "NewRoom");
    REQUIRE(map_info["rooms_data"].contains(key));
    const nlohmann::json& room = map_info["rooms_data"][key];
    CHECK(room.value("geometry", std::string{}) == "Square");
    CHECK(room["width"].is_object());
    CHECK(room["height"].is_object());
    CHECK(room["width"].value("random", false));
    CHECK(room["width"].value("center", 0) == 1500);
    CHECK(room["width"].value("span", 0) == 300);
    CHECK(room["height"].value("center", 0) == 1500);
    CHECK(room["height"].value("span", 0) == 300);
    CHECK(room.value("edge_smoothness", 0) == 4);
    CHECK(room["curvyness"].is_object());
    CHECK(room["spawn_groups"].is_array());
}

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
