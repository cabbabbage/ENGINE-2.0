#include <doctest/doctest.h>

#include <algorithm>
#include <nlohmann/json.hpp>

#include "config/room_config/room_configurator.hpp"

namespace {

nlohmann::json square_room_fixture() {
    return nlohmann::json::object({
        {"name", "Alpha"},
        {"geometry", "Square"},
        {"min_width", 480},
        {"max_width", 960},
        {"min_height", 512},
        {"max_height", 1024},
        {"edge_smoothness", 4},
        {"curvyness", 3},
        {"is_boss", false},
        {"inherits_map_assets", true},
        {"camera_height_px", 1450},
        {"camera_tilt_deg", 27.5f},
        {"camera_zoom_percent", 125}
    });
}

nlohmann::json circle_room_fixture() {
    return nlohmann::json::object({
        {"name", "Round"},
        {"geometry", "Circle"},
        {"radius", 320},
        {"min_radius", 240},
        {"max_radius", 320},
        {"edge_smoothness", 2},
        {"curvyness", 1},
        {"is_boss", true},
        {"inherits_map_assets", false}
    });
}

}  // namespace

TEST_CASE("RoomConfigurator compact mode preserves room metadata and existing camera keys") {
    RoomConfigurator configurator;
    configurator.set_room_metadata_only_mode(true);

    const nlohmann::json source = square_room_fixture();
    configurator.open(source);

    const nlohmann::json saved = configurator.build_json();
    CHECK(saved.value("name", std::string{}) == "Alpha");
    CHECK(saved.value("geometry", std::string{}) == "Square");
    CHECK(saved.value("min_width", 0) == 480);
    CHECK(saved.value("max_width", 0) == 960);
    CHECK(saved.value("min_height", 0) == 512);
    CHECK(saved.value("max_height", 0) == 1024);
    CHECK(saved.value("edge_smoothness", 0) == 4);
    CHECK(saved.value("curvyness", 0) == 3);
    CHECK_FALSE(saved.value("is_boss", true));
    CHECK(saved.value("inherits_map_assets", false));
    CHECK_FALSE(saved.contains("tags"));
    CHECK_FALSE(saved.contains("anti_tags"));
    CHECK(saved.value("camera_height_px", 0) == 1450);
    CHECK(saved.value("camera_tilt_deg", 0.0f) == doctest::Approx(27.5f));
    CHECK(saved.value("camera_zoom_percent", 0) == 125);
}

TEST_CASE("RoomConfigurator compact mode migrates legacy radius to width and height bounds") {
    RoomConfigurator configurator;
    configurator.set_room_metadata_only_mode(true);

    const nlohmann::json source = circle_room_fixture();
    configurator.open(source);

    const nlohmann::json saved = configurator.build_json();
    CHECK(saved.value("name", std::string{}) == "Round");
    CHECK(saved.value("geometry", std::string{}) == "Circle");
    CHECK(saved.value("min_width", 0) == 480);
    CHECK(saved.value("max_width", 0) == 640);
    CHECK(saved.value("min_height", 0) == 480);
    CHECK(saved.value("max_height", 0) == 640);
    CHECK_FALSE(saved.contains("radius"));
    CHECK_FALSE(saved.contains("min_radius"));
    CHECK_FALSE(saved.contains("max_radius"));
    CHECK(saved.value("edge_smoothness", 0) == 2);
    CHECK(saved.value("curvyness", 0) == 1);
    CHECK(saved.value("is_boss", false));
    CHECK_FALSE(saved.value("inherits_map_assets", true));
}

TEST_CASE("RoomConfigurator compact mode self-heals malformed dimensions and preserves existing canonical bounds") {
    RoomConfigurator configurator;
    configurator.set_room_metadata_only_mode(true);

    nlohmann::json malformed = nlohmann::json::object({
        {"name", "Broken"},
        {"geometry", "Circle"},
        {"min_width", -25},
        {"max_width", "oops"},
        {"min_height", 9000},
        {"max_height", 2},
        {"radius", 111},
        {"min_radius", 10},
        {"max_radius", 11},
        {"edge_smoothness", 999}
    });

    configurator.open(malformed);
    const nlohmann::json saved = configurator.build_json();

    CHECK(saved.value("min_width", 0) >= 1);
    CHECK(saved.value("max_width", 0) >= saved.value("min_width", 0));
    CHECK(saved.value("min_height", 0) >= 1);
    CHECK(saved.value("max_height", 0) >= saved.value("min_height", 0));
    CHECK(saved.value("max_height", 0) <= 4000);
    CHECK(saved.value("edge_smoothness", 0) == 101);
    CHECK_FALSE(saved.contains("radius"));
    CHECK_FALSE(saved.contains("min_radius"));
    CHECK_FALSE(saved.contains("max_radius"));
}

TEST_CASE("RoomConfigurator compact mode persists trail connection sector defaults when missing") {
    RoomConfigurator configurator;
    configurator.set_room_metadata_only_mode(true);

    nlohmann::json source = square_room_fixture();
    source.erase("trail_connection_sector");
    configurator.open(source);

    const nlohmann::json saved = configurator.build_json();
    REQUIRE(saved.contains("trail_connection_sector"));
    REQUIRE(saved["trail_connection_sector"].is_object());
    CHECK(saved["trail_connection_sector"].value("direction_deg", -1.0) == doctest::Approx(0.0));
    CHECK(saved["trail_connection_sector"].value("width_percent", -1) == 100);
}

TEST_CASE("RoomConfigurator compact mode normalizes trail connection sector values") {
    RoomConfigurator configurator;
    configurator.set_room_metadata_only_mode(true);

    nlohmann::json source = square_room_fixture();
    source["trail_connection_sector"] = nlohmann::json::object({
        {"direction_deg", -90.0},
        {"width_percent", 1}
    });
    configurator.open(source);

    const nlohmann::json saved = configurator.build_json();
    REQUIRE(saved.contains("trail_connection_sector"));
    REQUIRE(saved["trail_connection_sector"].is_object());
    CHECK(saved["trail_connection_sector"].value("direction_deg", -1.0) == doctest::Approx(270.0));
    CHECK(saved["trail_connection_sector"].value("width_percent", -1) == 25);
}

TEST_CASE("RoomConfigurator room context writes canonical room_tags and drops anti-tag fields") {
    RoomConfigurator configurator;

    nlohmann::json source = square_room_fixture();
    source["room_tags"] = nlohmann::json::array({"combat", "treasure"});
    source["tags"] = nlohmann::json::object({
        {"include", nlohmann::json::array({"legacy"})},
        {"exclude", nlohmann::json::array({"deprecated"})}
    });
    source["anti_tags"] = nlohmann::json::array({"not_used"});

    configurator.open(source);
    const nlohmann::json saved = configurator.build_json();

    REQUIRE(saved.contains("room_tags"));
    REQUIRE(saved["room_tags"].is_array());
    CHECK(std::find(saved["room_tags"].begin(), saved["room_tags"].end(), "combat") != saved["room_tags"].end());
    CHECK(std::find(saved["room_tags"].begin(), saved["room_tags"].end(), "treasure") != saved["room_tags"].end());
    CHECK_FALSE(saved.contains("tags"));
    CHECK_FALSE(saved.contains("anti_tags"));
}
