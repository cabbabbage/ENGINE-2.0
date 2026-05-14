#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "devtools/config/room_config/room_configurator.hpp"
#include "gameplay/map_generation/room.hpp"
#include "utils/area.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/weighted_range.hpp"

namespace {

nlohmann::json flat_range(int value) {
    return vibble::weighted_range::to_json(vibble::weighted_range::make_flat(value));
}

nlohmann::json uniform_range(int min_value, int max_value) {
    return vibble::weighted_range::to_json(vibble::weighted_range::make_legacy_uniform(min_value, max_value));
}

std::unique_ptr<Room> make_config_room(const std::string& type,
                                       const std::string& name,
                                       const std::string& data_section,
                                       nlohmann::json data) {
    std::vector<SDL_Point> points{
        SDL_Point{0, 0},
        SDL_Point{100, 0},
        SDL_Point{100, 100},
        SDL_Point{0, 100},
    };
    Area area(name, points, 3);
    auto room = std::make_unique<Room>(
        Room::Point{0, 0},
        type,
        name,
        nullptr,
        "test_map",
        nullptr,
        &area,
        nullptr,
        MapGridSettings::defaults(),
        3000.0,
        data_section,
        nullptr,
        nullptr,
        std::string{},
        Room::ManifestWriter{},
        false);
    room->assets_data() = std::move(data);
    return room;
}

std::unique_ptr<Room> make_room(nlohmann::json data, const std::string& name = "room_template") {
    return make_config_room("room", name, "rooms_data", std::move(data));
}

std::unique_ptr<Room> make_trail_room(nlohmann::json data) {
    return make_config_room("trail", "trail_template", "trails_data", std::move(data));
}

}  // namespace

TEST_CASE("room configurator canonical room roundtrip preserves owned and unrelated data") {
    nlohmann::json input = nlohmann::json::object({
        {"name", "forest"},
        {"geometry", "Square"},
        {"width", uniform_range(1200, 1600)},
        {"height", uniform_range(800, 900)},
        {"curvyness", flat_range(7)},
        {"edge_smoothness", 33},
        {"is_boss", true},
        {"inherits_live_dynamic_assets", true},
        {"inherit_map_floor_color", false},
        {"room_floor_color", nlohmann::json::array({11, 22, 33})},
        {"trail_connection_sector", nlohmann::json::object({{"direction_deg", 450.0}, {"width_percent", 80}})},
        {"room_tags", nlohmann::json::array({"forest", "shop"})},
        {"spawn_groups", nlohmann::json::array({nlohmann::json::object({{"id", "keep_spawn"}})})},
        {"camera_height_px", 777},
        {"custom_metadata", nlohmann::json::object({{"keep", true}})},
    });

    RoomConfigurator configurator;
    configurator.open(input);
    const nlohmann::json output = configurator.build_json();

    CHECK(output["width"] == input["width"]);
    CHECK(output["height"] == input["height"]);
    CHECK(output["curvyness"] == input["curvyness"]);
    CHECK(output.value("edge_smoothness", 0) == 33);
    CHECK(output.value("is_boss", false));
    CHECK(output.value("inherits_live_dynamic_assets", false));
    CHECK_FALSE(output.contains("inherits_map_assets"));
    CHECK(output.value("inherit_map_floor_color", true) == false);
    CHECK(output["room_floor_color"] == nlohmann::json::array({11, 22, 33}));
    CHECK(output["trail_connection_sector"].value("direction_deg", 0.0) == doctest::Approx(90.0));
    CHECK(output["trail_connection_sector"].value("width_percent", 0) == 80);
    CHECK(output["room_tags"] == nlohmann::json::array({"forest", "shop"}));
    CHECK(output["spawn_groups"] == input["spawn_groups"]);
    CHECK(output.value("camera_height_px", 0) == 777);
    CHECK(output["custom_metadata"] == input["custom_metadata"]);
}

TEST_CASE("room configurator migrates legacy room metadata to canonical fields") {
    nlohmann::json input = nlohmann::json::object({
        {"name", "legacy"},
        {"geometry", "Square"},
        {"min_width", 100},
        {"max_width", 200},
        {"min_height", 300},
        {"max_height", 500},
        {"curviness", 4},
        {"inherits_map_assets", true},
    });

    RoomConfigurator configurator;
    configurator.open(input);
    const nlohmann::json output = configurator.build_json();

    CHECK(output["width"] == uniform_range(100, 200));
    CHECK(output["height"] == uniform_range(300, 500));
    CHECK(output["curvyness"] == flat_range(4));
    CHECK(output.value("inherits_live_dynamic_assets", false));
    CHECK_FALSE(output.contains("inherits_map_assets"));
    CHECK_FALSE(output.contains("min_width"));
    CHECK_FALSE(output.contains("max_width"));
    CHECK_FALSE(output.contains("min_height"));
    CHECK_FALSE(output.contains("max_height"));
    CHECK_FALSE(output.contains("curviness"));
}

TEST_CASE("room configurator keeps circle room height tied to width") {
    nlohmann::json input = nlohmann::json::object({
        {"name", "circle"},
        {"geometry", "Circle"},
        {"width", uniform_range(700, 900)},
        {"height", uniform_range(100, 200)},
    });

    RoomConfigurator configurator;
    configurator.open(input);
    const nlohmann::json output = configurator.build_json();

    CHECK(output["height"] == output["width"]);
}

TEST_CASE("room configurator trail roundtrip preserves independent height and omits room-only sector") {
    nlohmann::json input = nlohmann::json::object({
        {"name", "trail_template"},
        {"geometry", "Square"},
        {"width", uniform_range(120, 180)},
        {"height", uniform_range(40, 70)},
        {"curvyness", flat_range(9)},
        {"edge_smoothness", 12},
        {"inherits_live_dynamic_assets", true},
        {"inherit_map_floor_color", false},
        {"room_floor_color", nlohmann::json::array({44, 55, 66})},
        {"trail_connection_sector", nlohmann::json::object({{"direction_deg", 180.0}, {"width_percent", 50}})},
        {"tags", nlohmann::json::object({{"include", nlohmann::json::array({"trail"})},
                                         {"exclude", nlohmann::json::array({"blocked"})}})},
        {"custom_metadata", nlohmann::json::object({{"keep", "yes"}})},
    });

    std::unique_ptr<Room> trail = make_trail_room(input);
    RoomConfigurator configurator;
    configurator.open(trail.get());
    const nlohmann::json output = configurator.build_json();

    CHECK(output["width"] == input["width"]);
    CHECK(output["height"] == input["height"]);
    CHECK(output["curvyness"] == input["curvyness"]);
    CHECK(output.value("edge_smoothness", 0) == 12);
    CHECK(output.value("inherits_live_dynamic_assets", false));
    CHECK_FALSE(output.contains("inherits_map_assets"));
    CHECK(output.value("inherit_map_floor_color", true) == false);
    CHECK(output["room_floor_color"] == nlohmann::json::array({44, 55, 66}));
    CHECK_FALSE(output.contains("trail_connection_sector"));
    CHECK_FALSE(output.contains("is_boss"));
    CHECK(output["tags"]["include"] == nlohmann::json::array({"trail"}));
    CHECK(output["tags"]["exclude"] == nlohmann::json::array({"blocked"}));
    CHECK(output["custom_metadata"] == input["custom_metadata"]);
}

TEST_CASE("room configurator open Room pointer reloads each room json") {
    nlohmann::json first_input = nlohmann::json::object({
        {"name", "first"},
        {"geometry", "Square"},
        {"width", uniform_range(100, 200)},
        {"height", uniform_range(300, 400)},
        {"curvyness", flat_range(2)},
        {"edge_smoothness", 5},
    });
    nlohmann::json second_input = nlohmann::json::object({
        {"name", "second"},
        {"geometry", "Square"},
        {"width", uniform_range(700, 900)},
        {"height", uniform_range(1100, 1300)},
        {"curvyness", flat_range(8)},
        {"edge_smoothness", 44},
        {"inherits_live_dynamic_assets", true},
    });

    std::unique_ptr<Room> first = make_room(first_input, "first");
    std::unique_ptr<Room> second = make_room(second_input, "second");

    RoomConfigurator configurator;
    configurator.open(first.get());
    configurator.open(second.get());
    const nlohmann::json output = configurator.build_json();

    CHECK(output.value("name", std::string{}) == "second");
    CHECK(output["width"] == second_input["width"]);
    CHECK(output["height"] == second_input["height"]);
    CHECK(output["curvyness"] == second_input["curvyness"]);
    CHECK(output.value("edge_smoothness", 0) == 44);
    CHECK(output.value("inherits_live_dynamic_assets", false));
}

TEST_CASE("room configurator live room uses actual room name over copied template metadata") {
    nlohmann::json input = nlohmann::json::object({
        {"name", "baseball"},
        {"room_name", "baseball"},
        {"geometry", "Square"},
        {"width", uniform_range(700, 900)},
        {"height", uniform_range(1100, 1300)},
        {"curvyness", flat_range(8)},
        {"edge_smoothness", 44},
    });

    std::unique_ptr<Room> room = make_room(input, "Loot_Room");

    RoomConfigurator configurator;
    configurator.open(room.get());
    const nlohmann::json output = configurator.build_json();

    CHECK(configurator.current_header_text() == "Room: Loot_Room");
    CHECK(output.value("name", std::string{}) == "Loot_Room");
    CHECK_FALSE(output.contains("room_name"));
}

TEST_CASE("room configurator external trail template opens in trail context") {
    nlohmann::json input = nlohmann::json::object({
        {"name", "external_trail"},
        {"geometry", "Square"},
        {"width", uniform_range(300, 500)},
        {"height", uniform_range(90, 120)},
        {"curvyness", flat_range(6)},
        {"edge_smoothness", 17},
        {"is_boss", true},
        {"trail_connection_sector", nlohmann::json::object({{"direction_deg", 45.0}, {"width_percent", 25}})},
        {"tags", nlohmann::json::object({{"include", nlohmann::json::array({"trail"})},
                                         {"exclude", nlohmann::json::array({"blocked"})}})},
    });

    bool changed = false;
    RoomConfigurator configurator;
    configurator.set_room_metadata_only_mode(true);
    configurator.open(input, true, [&changed]() { changed = true; });
    const nlohmann::json output = configurator.build_json();

    CHECK(changed);
    CHECK(output["width"] == uniform_range(300, 500));
    CHECK(output["height"] == uniform_range(90, 120));
    CHECK(output["curvyness"] == flat_range(6));
    CHECK(output.value("edge_smoothness", 0) == 17);
    CHECK_FALSE(output.contains("trail_connection_sector"));
    CHECK_FALSE(output.contains("is_boss"));
    CHECK(output["tags"]["include"] == nlohmann::json::array({"trail"}));
    CHECK(output["tags"]["exclude"] == nlohmann::json::array({"blocked"}));
    CHECK_FALSE(input.contains("trail_connection_sector"));
    CHECK_FALSE(input.contains("is_boss"));
}
