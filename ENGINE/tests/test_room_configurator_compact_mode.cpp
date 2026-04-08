#include <doctest/doctest.h>

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
        {"is_spawn", true},
        {"is_boss", false},
        {"inherits_map_assets", true}
    });
}

nlohmann::json circle_room_fixture() {
    return nlohmann::json::object({
        {"name", "Round"},
        {"geometry", "Circle"},
        {"radius", 320},
        {"min_radius", 240},
        {"max_radius", 320},
        {"min_width", 480},
        {"max_width", 640},
        {"min_height", 480},
        {"max_height", 640},
        {"edge_smoothness", 2},
        {"curvyness", 1},
        {"is_spawn", false},
        {"is_boss", true},
        {"inherits_map_assets", false}
    });
}

}  // namespace

TEST_CASE("RoomConfigurator compact mode preserves room metadata and omits tags and camera keys") {
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
    CHECK(saved.value("is_spawn", false));
    CHECK_FALSE(saved.value("is_boss", true));
    CHECK(saved.value("inherits_map_assets", false));
    CHECK_FALSE(saved.contains("tags"));
    CHECK_FALSE(saved.contains("anti_tags"));
    CHECK_FALSE(saved.contains("camera_height_px"));
    CHECK_FALSE(saved.contains("camera_tilt_deg"));
    CHECK_FALSE(saved.contains("camera_zoom_percent"));
}

TEST_CASE("RoomConfigurator compact mode round-trips circular room dimensions") {
    RoomConfigurator configurator;
    configurator.set_room_metadata_only_mode(true);

    const nlohmann::json source = circle_room_fixture();
    configurator.open(source);

    const nlohmann::json saved = configurator.build_json();
    CHECK(saved.value("name", std::string{}) == "Round");
    CHECK(saved.value("geometry", std::string{}) == "Circle");
    CHECK(saved.value("radius", 0) == 320);
    CHECK(saved.value("min_radius", 0) == 240);
    CHECK(saved.value("max_radius", 0) == 320);
    CHECK(saved.value("min_width", 0) == 480);
    CHECK(saved.value("max_width", 0) == 640);
    CHECK(saved.value("min_height", 0) == 480);
    CHECK(saved.value("max_height", 0) == 640);
    CHECK(saved.value("edge_smoothness", 0) == 2);
    CHECK(saved.value("curvyness", 0) == 1);
    CHECK_FALSE(saved.value("is_spawn", true));
    CHECK(saved.value("is_boss", false));
    CHECK_FALSE(saved.value("inherits_map_assets", true));
}
